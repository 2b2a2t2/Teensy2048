# 7III Tap 2.0.1

from __future__ import with_statement
import Live
from _Framework.ControlSurface import ControlSurface
from _Framework.MixerComponent import MixerComponent
from _Framework.TransportComponent import TransportComponent
from _Framework.SessionComponent import SessionComponent
from _Framework.EncoderElement import *
from _Framework.ButtonElement import ButtonElement
from _Framework.SliderElement import SliderElement
from _Framework.InputControlElement import MIDI_NOTE_TYPE, MIDI_NOTE_ON_STATUS, MIDI_NOTE_OFF_STATUS, MIDI_CC_TYPE
from _Framework.DeviceComponent import DeviceComponent
from ableton.v2.base import listens, liveobj_valid, liveobj_changed
from Live.Clip import MidiNoteSpecification

import threading
import random
import re
import math
from itertools import zip_longest
import time

secret_version_number = 8

mixer, transport, session_component = None, None, None
quantize_grid_value = 5
quantize_strength_value = 1.0
swing_amount_value = 0.0


class TapDeviceComponent(DeviceComponent):
    SAFE_PARAMETER_BANK_SIZE = 8

    def __init__(self, *a, **k):
        DeviceComponent.__init__(self, *a, **k)
        self._use_safe_parameter_banks = False

    def set_device(self, device):
        self._use_safe_parameter_banks = False
        try:
            return DeviceComponent.set_device(self, device)
        except IndexError:
            self._use_safe_parameter_banks = True
            self._bank_index = 0
            try:
                self.update()
            except Exception:
                pass
            try:
                self.notify_device()
            except Exception:
                pass

    def update(self):
        try:
            return DeviceComponent.update(self)
        except IndexError:
            self._use_safe_parameter_banks = True
            self._clamp_bank_index_to_safe_banks()
            try:
                return DeviceComponent.update(self)
            except IndexError:
                pass

    def _current_bank_details(self):
        try:
            return DeviceComponent._current_bank_details(self)
        except IndexError:
            self._use_safe_parameter_banks = True
            self._clamp_bank_index_to_safe_banks()
            try:
                return DeviceComponent._current_bank_details(self)
            except IndexError:
                return '', tuple([None] * self.SAFE_PARAMETER_BANK_SIZE)

    def _parameter_banks(self):
        if self._use_safe_parameter_banks:
            return self._safe_parameter_banks()
        try:
            return DeviceComponent._parameter_banks(self)
        except IndexError:
            self._use_safe_parameter_banks = True
            return self._safe_parameter_banks()

    def _parameter_bank_names(self):
        if self._use_safe_parameter_banks:
            return self._safe_parameter_bank_names()
        try:
            return DeviceComponent._parameter_bank_names(self)
        except IndexError:
            self._use_safe_parameter_banks = True
            return self._safe_parameter_bank_names()

    def _best_of_parameter_bank(self):
        if self._use_safe_parameter_banks:
            return []
        try:
            return DeviceComponent._best_of_parameter_bank(self)
        except IndexError:
            self._use_safe_parameter_banks = True
            return []

    def _number_of_parameter_banks(self):
        if self._use_safe_parameter_banks:
            return len(self._safe_parameter_banks())
        try:
            return DeviceComponent._number_of_parameter_banks(self)
        except IndexError:
            self._use_safe_parameter_banks = True
            return len(self._safe_parameter_banks())

    def _clamp_bank_index_to_safe_banks(self):
        safe_banks = self._safe_parameter_banks()
        bank_count = len(safe_banks)
        if bank_count == 0:
            self._bank_index = 0
        else:
            self._bank_index = max(0, min(self._bank_index, bank_count - 1))

    def _safe_parameter_banks(self):
        device = getattr(self, '_device', None)
        if not device or not liveobj_valid(device) or not hasattr(device, 'parameters'):
            return []

        parameters = list(device.parameters)
        if not parameters:
            return []

        live_banks = self._safe_live_parameter_banks(device, parameters)
        if live_banks:
            return live_banks

        parameters = parameters[1:]
        if not parameters:
            return []

        banks = []
        for index in range(0, len(parameters), self.SAFE_PARAMETER_BANK_SIZE):
            bank = list(parameters[index:index + self.SAFE_PARAMETER_BANK_SIZE])
            bank.extend([None] * (self.SAFE_PARAMETER_BANK_SIZE - len(bank)))
            banks.append(tuple(bank))
        return banks

    def _safe_live_parameter_banks(self, device, parameters):
        if not hasattr(device, 'get_bank_count') or not hasattr(device, 'get_bank_parameters'):
            return []

        try:
            bank_count = int(device.get_bank_count())
        except Exception:
            bank_count = 0

        if bank_count <= 0:
            return []

        banks = []
        empty_bank = tuple([None] * self.SAFE_PARAMETER_BANK_SIZE)
        for bank_index in range(bank_count):
            try:
                parameter_indices = list(device.get_bank_parameters(bank_index))
            except Exception:
                parameter_indices = []

            if len(parameter_indices) != self.SAFE_PARAMETER_BANK_SIZE:
                banks.append(empty_bank)
                continue

            bank = []
            for parameter_index in parameter_indices:
                if parameter_index == -1:
                    bank.append(None)
                elif 0 <= parameter_index < len(parameters):
                    bank.append(parameters[parameter_index])
                else:
                    bank.append(None)
            banks.append(tuple(bank))

        return banks

    def _safe_parameter_bank_names(self):
        bank_count = len(self._safe_parameter_banks())
        if bank_count == 0:
            return []

        device = getattr(self, '_device', None)
        names = []
        for index in range(bank_count):
            name = None
            if device and hasattr(device, 'get_bank_name'):
                try:
                    name = device.get_bank_name(index)
                except Exception:
                    name = None
            if name:
                name = ''.join(char for char in str(name) if ord(char) < 128)
            names.append(name or "Bank {}".format(index + 1))
        return tuple(names)


class Tap(ControlSurface):
    SYSEX_STRING_ESCAPE_CHAR = "\\"
    SYSEX_STRING_RESERVED_SYMBOLS = (",", "|", ";", "^", "-", "%", ":", "/", "<", "*", "$", "_", "&", "(", ")", "\\")
    MUTATOR_GENERATION_COOLDOWN_SECONDS = 1.25
    FOLLOW_ACTION_NAME_MARKER_RE = re.compile(r"\s*\[TapFA:v1\|([^\]]*)\]")
    DECOUPLED_AUTOMATION_NAME_MARKER_RE = re.compile(r"\s*\[TapAuto:v2\|([^\]]*)\]")
    DECOUPLED_AUTOMATION_ANY_NAME_MARKER_RE = re.compile(r"\s*\[TapAuto:v[0-9]+\|([^\]]*)\]")
    MUTATOR_NAME_MARKER_RE = re.compile(r"\s*\[(?:TapComp|TapMut):v1\|([^\]]*)\]")
    MUTATOR_ANY_NAME_MARKER_RE = re.compile(r"\s*\[(?:TapComp|TapMut):v[0-9]+\|([^\]]*)\]")
    SYSEX_TEXT_SYMBOL_REPLACEMENTS = (
        ('♭', 'b'),
        ('♯', '#'),
        ('♮', 'nat'),
        ('𝄫', 'bb'),
        ('𝄪', '##'),
    )
    SYSEX_ROUND_BRACKET_TEXT_RE = re.compile(r'\(([^()]*)\)')
    SYSEX_SQUARE_BRACKET_TEXT_RE = re.compile(r'\[([^\[\]]*)\]')
    SYSEX_CURLY_BRACKET_TEXT_RE = re.compile(r'\{([^{}]*)\}')
    SYSEX_MULTI_SPACE_RE = re.compile(r'\s{2,}')
    MUTATOR_ALGORITHMS = (
        "mutator",
        "verse_weaver",
        "motif_ladder",
        "sparse_echo",
        "chorus_lift",
        "middle_eight",
        "tension_break",
        "skylight_hook",
        "glass_steps",
        "nocturne_line",
        "modal_drift",
        "circle_resolve",
        "backbeat_engine",
        "broken_garage",
        "four_floor_bloom",
    )
    MUTATOR_SCALE_NAMES = (
        "Chromatic",
        "Major",
        "Minor",
        "Dorian",
        "Mixolydian",
        "Lydian",
        "Phrygian",
        "Locrian",
        "Whole Tone",
        "Half-whole Dim.",
        "Whole-half Dim.",
        "Minor Blues",
        "Minor Pentatonic",
        "Major Pentatonic",
        "Harmonic Minor",
        "Harmonic Major",
        "Dorian #4",
        "Phrygian Dominant",
        "Melodic Minor",
        "Lydian Augmented",
        "Lydian Dominant",
        "Super Locrian",
        "8-Tone Spanish",
        "Bhairav",
        "Hungarian Minor",
        "Hirajoshi",
        "In-Sen",
        "Iwato",
        "Kumoi",
        "Pelog Selisir",
        "Pelog Tembung",
        "Messiaen 3",
        "Messiaen 4",
        "Messiaen 5",
        "Messiaen 6",
        "Messiaen 7",
    )
    DECOUPLED_AUTOMATION_MAX_PHYSICAL_BARS = 16
    PARAMETER_DISPLAY_FEEDBACK_INTERVAL = 0.03
    CHUNKED_INCOMING_SYSEX_IDS = (14, 15, 16, 35, 36, 49, 50, 51, 55, 57, 58)
    DISPLAY_VALUE_NUMBER_PATTERN = re.compile(r'(?<![\d.])([+-]?\d+)\.(\d+)(?![\d.])')
    PARAMETER_METADATA_RECHECK_INTERVAL = 0.1
    PARAMETER_METADATA_RECHECK_DURATION = 1.2
    UNMAPPED_PARAMETER_METADATA_ITEM = "*--&&-|0|127|0.0|0.0|32|"
    UNMAPPED_PARAMETER_METADATA = ",".join([UNMAPPED_PARAMETER_METADATA_ITEM] * 8)
    TRACK_DEVICE_NAV_NAME = "line.3.horizontal"
    TRACK_DEVICE_MAIN_BANK_NAME = "Main"
    TRACK_DEVICE_SEND_BANK_NAME = "Sends"
    TRACK_DEVICE_BANK_SIZE = 8
    TRACK_DEVICE_PITCH_BEND_CENTER = 8192.0
    TRACK_DEVICE_PITCH_BEND_MAX = 16383.0
    TRACK_DEVICE_MIDI_CONTROLS = (
        {"name": "Mod Wheel", "kind": "mod_wheel", "min": 0.0, "max": 127.0, "default": 0.0, "automatable": False},
        {"name": "Pressure", "kind": "pressure", "min": 0.0, "max": 127.0, "default": 0.0, "automatable": False},
        {"name": "Pitch Bend", "kind": "pitch_bend", "min": 0.0, "max": 16383.0, "default": 8192.0, "automatable": False},
        {"name": "Velocity", "kind": "velocity", "min": 1.0, "max": 127.0, "default": 100.0, "automatable": False},
    )
    AUTOMATION_ENVELOPE_MAX_SAMPLES = 1024
    AUTOMATION_ENVELOPE_LINEAR_EPSILON = 0.0015
    AUTOMATION_ENVELOPE_JUMP_THRESHOLD = 0.1
    AUTOMATION_FOLDED_ENDPOINT_ORDER = 2147483647
    

    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        with self.component_guard():
            global mixer
            global transport
            global session_component
            self.mixer_status = False
            self.mixer_reset = True
            self.visible_channels = (0, 3)
            self.device_status = True
            self.seq_status = False
            self.seq_clip_playing_status = 2
            track_count = 127
            return_count = 12  # Maximum of 12 Sends and 12 Returns
            max_clip_slots = 800  # Adjust this number based on your needs
            self.playing_position_listeners = [None] * max_clip_slots
            self.current_clip_notes = []
            self.last_selected_clip_slot = None
            self.last_raw_notes = None
            self.currently_playing_notes = [False] * 128
            self.last_playing_position = 0.0
            self.last_sent_out_playing_pos = 0.0
            self._last_sent_playing_pos_cc_pair = None
            self.clip_length_trick = 110.0
            mixer = MixerComponent(track_count, return_count)
            transport = TransportComponent()
            session_component = SessionComponent()
            self.old_clips_array = []
            self._drum_rack_device = None
            self.was_initialized = False
            self._track_level_listeners = {}
            self._return_level_listeners = {}
            self._master_level_listeners = {}
            self._mixer_meter_targets = {}
            self._disabled_parameter_listeners = {}
            self._disabled_parameters = []
            self._current_disabled_controls = []
            self._automation_state_listeners = {}
            self._automation_metadata_update_timer = None
            self._automation_metadata_retry_count = 0
            self._automation_metadata_retry_start = None
            self._last_automation_signature = None
            # browser pagination state
            self.browser_current_items = []
            self.browser_current_page = 0
            self.browser_pages_count = 0
            self.browser_items_per_page = 12
            self.browser_insert_after_device_index = None
            # browser navigation history for back button
            self.browser_history = []
            self.browser_folder_mapping = {
                0: 'audio_effects',
                1: 'colors',
                2: 'current_project',
                3: 'drums',
                4: 'instruments',
                5: 'max_for_live',
                6: 'midi_effects',
                7: 'packs',
                8: 'plugins',
                9: 'sounds',
                10: 'user_folders',
                11: 'user_library'
            }
            self._metadata_recheck_timer = None
            self._last_sent_metadata = None
            self._last_drum_pad_metadata = None
            self._drum_pad_change_recheck_count = 0
            self._drum_pad_recheck_start = None
            self._last_drum_pad_note = None
            self._last_drum_pad_change_at = 0.0
            self._device_recheck_start = None
            self._device_recheck_count = 0
            self._debug_mode = False
            self._metadata_cache = {}
            self._metadata_send_seq = 0
            self._metadata_send_seq_by_device = {}
            self._automation_metadata_device_id = None
            self._automation_timer_lock = threading.Lock()
            self._mixer_disconnect_timer = None
            self._follow_action_rules = {}
            self._active_follow_actions = {}
            self._handled_follow_action_launches = set()
            self._active_high_resolution_gestures = set()
            self._active_high_resolution_undo_steps = set()
            self._parameter_value_listeners = {}
            self._parameter_name_listeners = {}
            self._parameter_name_update_timer = None
            self._bank_metadata_refresh_timers = []
            self._parameter_source_device = None
            self._parameter_source_listener = None
            self._bank_parameter_source_device = None
            self._bank_parameter_source_listener = None
            self._last_sent_parameter_displays = {}
            self._last_sent_parameter_normalized_values = {}
            self._last_sent_parameter_cc_values = {}
            self._last_parameter_display_feedback_times = {}
            self._follow_action_track_signature = None
            self._follow_action_missing_clip_counts = {}
            self._last_follow_action_state = None
            self._last_song_is_playing = False
            self._last_sent_transport_state = None
            self._last_sent_session_record_state = None
            self._follow_action_scene_triggered_listeners = {}
            self._follow_action_clip_slot_runtime_listeners = {}
            self._follow_action_scene_name_listeners = {}
            self._follow_action_clip_name_listeners = {}
            self._follow_action_clip_has_clip_listeners = {}
            self._follow_action_clip_timing_listeners = {}
            self._follow_action_song_listener_subject = None
            self._mutator_regeneration_states = {}
            self._mutator_generation_in_progress = set()
            self._mutator_generation_scheduled = set()
            self._queued_mutator_work = {}
            self._last_mutator_generation_times = {}
            self._last_mutator_generation_request_times = {}
            self._last_mutator_generation_signatures = {}
            self._mutator_playing_clip_keys = set()
            self._mutator_triggered_clip_keys = set()
            self._mutator_generation_lock = threading.RLock()
            self._last_mutator_scale_root_signature = None
            self._mutator_scale_root_sync_scheduled = False
            self._selected_clip_update_suppression_depth = 0
            self._selected_clip_update_pending_metadata = False
            self._selected_clip_update_pending_notes = False
            self._clip_slot_listeners = {}
            self._registered_track_ids = set()
            self._clip_color_listeners = {}
            self._clip_listener_track_slots = {}
            self._clip_slot_color_map = {}
            self._track_list_signature = None
            self._last_group_fold_states = None
            self._last_group_hidden_states = None
            self._previous_selected_track = None
            self._periodic_timer_ref = None
            self._smooth_macro_randomize_token = 0
            self._smooth_macro_randomize_state = None
            self._re_enable_automation_enabled_state = None
            self._remove_automation_from_next_encoder = False
            self._automation_parameter_action = None
            self._automation_removal_suppressed_controls = set()
            self._automation_authored_steps = {}
            self._mixer_automation_controls = []
            self._mixer_automation_status_specs = []
            self._mixer_automation_state_listeners = []
            self._mixer_automation_status_timers = []
            self._track_device_selected = False
            self._track_control_selection_by_track = {}
            self._track_control_bank_index_by_track = {}
            self._track_midi_control_values_by_track = {}
            self.periodic_timer = 1
            # connection check button
            connection_check_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 94)
            connection_check_button.add_value_listener(self._connection_established)
            self.transport_toggle_button = ButtonElement(1, MIDI_CC_TYPE, 0, 116)
            self.transport_toggle_button.add_value_listener(self._transport_toggle_value)
            # send project again button
            send_project_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 88)
            send_project_button.add_value_listener(self._send_project)

            # making a song instance
            self.song_instance = self.song()
            
            self._last_playing_pos_sent = 0.0
            self._ensure_follow_action_song_listeners(self.song_instance)
            self._sync_follow_action_name_listeners()
            self._load_follow_actions_from_names(force_send=True)
            self._sync_follow_action_runtime_listeners()
            self._start_periodic_execution()

    def _setup_device_control(self):
        self._device = TapDeviceComponent()
        self._device.name = 'Device_Component'
        
        self._device_controls = []
        for index in range(8):
            control = EncoderElement(MIDI_CC_TYPE, 8, 72 + index, Live.MidiMap.MapMode.absolute)
            control.name = 'Ctrl_' + str(index)
            control.add_value_listener(lambda value, control=control: self._on_device_control_value(value, control))
            self._device_controls.append(control)
        
        nav_left_button = ButtonElement(1, MIDI_CC_TYPE, 0, 33)
        nav_right_button = ButtonElement(1, MIDI_CC_TYPE, 0, 32)
        self._device.set_bank_nav_buttons(nav_left_button, nav_right_button)
        
        # Set up device change listener
        self._on_device_changed.subject = self._device
        self.set_device_component(self._device)
        
        # Register button listeners for navigation buttons
        nav_left_button.add_value_listener(self._on_nav_button_pressed)
        nav_right_button.add_value_listener(self._on_nav_button_pressed)
        
        # Initially connect the controls (assuming we start in device mode)
        self._connect_device_controls()

    def _connect_device_controls(self):
        if hasattr(self, '_device_controls'):
            for control in self._device_controls:
                try:
                    control.suppress_script_forwarding = False
                except Exception:
                    pass
            self._device.set_parameter_controls(self._device_controls)
        self._refresh_parameter_value_listeners_current_bank()
        self._refresh_parameter_name_listeners_current_bank()
        self._readd_disabled_parameter_listeners()
    
    def _disconnect_device_controls(self):
        if hasattr(self, '_device'):
            self._device.set_parameter_controls([])
        self._remove_parameter_value_listeners()
        self._remove_parameter_name_listeners()
        self._remove_disabled_parameter_listeners()
        self._remove_automation_state_listeners()

    def _connect_track_device_parameter_controls(self, track=None):
        if not hasattr(self, "_device") or not hasattr(self, "_device_controls"):
            return

        controls = []
        for control_index, control in enumerate(self._device_controls):
            entry = self._track_device_parameter_entry_for_control(control_index, track)
            is_parameter = bool(entry and entry.get("kind") == "parameter")
            try:
                control.suppress_script_forwarding = False
            except Exception:
                pass
            controls.append(control if is_parameter else None)
        self._device.set_parameter_controls(tuple(controls))

    def _on_nav_button_pressed(self, value):
        if value:
            self._on_device_changed(False)
            self._schedule_bank_metadata_refreshes()

    def _mapped_parameter_for_device_control(self, control_index):
        try:
            if not hasattr(self, '_device_controls') or control_index < 0 or control_index >= len(self._device_controls):
                return None
            control = self._device_controls[control_index]
            mapped_parameter = control.mapped_parameter() if control else None
            return mapped_parameter if mapped_parameter and liveobj_valid(mapped_parameter) else None
        except Exception:
            return None

    def _parameter_normalized_value(self, device_param):
        try:
            if not device_param or not hasattr(device_param, 'value') or not hasattr(device_param, 'min') or not hasattr(device_param, 'max'):
                return 0.0
            min_val = device_param.min
            max_val = device_param.max
            if max_val == min_val:
                return 0.0
            normalized = (device_param.value - min_val) / (max_val - min_val)
            return max(0.0, min(1.0, normalized))
        except Exception:
            return 0.0

    def _parameter_display_value(self, device_param):
        try:
            if device_param and hasattr(device_param, 'str_for_value') and hasattr(device_param, 'value'):
                return self._format_display_value_numbers(device_param.str_for_value(device_param.value).replace('∞', 'Inf'))
        except Exception:
            pass
        try:
            return self._format_display_value_numbers(str(device_param.value).replace('∞', 'Inf'))
        except Exception:
            return ""

    def _format_display_value_numbers(self, display_value):
        def format_match(match):
            integer_part = match.group(1)
            fractional_part = match.group(2)
            decimals = 1 if len(integer_part.lstrip('+-')) > 1 else 2
            if len(fractional_part) <= decimals:
                return match.group(0)
            try:
                value = float("{}.{}".format(integer_part, fractional_part))
                return "{:.{}f}".format(value, decimals)
            except Exception:
                return match.group(0)

        try:
            return self.DISPLAY_VALUE_NUMBER_PATTERN.sub(format_match, str(display_value))
        except Exception:
            return display_value

    def _parameter_target_value_from_normalized(self, device_param, normalized):
        min_val = device_param.min
        max_val = device_param.max
        if max_val == min_val:
            return min_val

        normalized = max(0.0, min(1.0, float(normalized)))

        return max(min_val, min(max_val, min_val + (max_val - min_val) * normalized))

    def _normalized_option_text(self, value):
        try:
            return str(value).strip().lower()
        except Exception:
            return ""

    def _parameter_current_value_item_index(self, device_param, item_count):
        display_value = self._normalized_option_text(self._parameter_display_value(device_param))
        if display_value and hasattr(device_param, 'value_items') and device_param.value_items:
            for index, item in enumerate(device_param.value_items):
                if self._normalized_option_text(item) == display_value:
                    return index

        try:
            normalized = self._parameter_normalized_value(device_param)
            return max(0, min(item_count - 1, int(math.floor(normalized * (item_count - 1) + 0.5))))
        except Exception:
            return 0

    def _set_device_control_next_option(self, control_index, device_param):
        if not hasattr(device_param, 'is_quantized') or not device_param.is_quantized:
            return
        if not hasattr(device_param, 'value_items') or not device_param.value_items:
            return

        item_count = len(device_param.value_items)
        if item_count <= 1:
            return

        current_index = self._parameter_current_value_item_index(device_param, item_count)
        next_index = (current_index + 1) % item_count
        normalized = float(next_index) / float(item_count - 1)
        target_value = self._parameter_target_value_from_normalized(device_param, normalized)

        if hasattr(device_param, 'begin_gesture'):
            device_param.begin_gesture()
        device_param.value = target_value
        if hasattr(device_param, 'end_gesture'):
            device_param.end_gesture()

        self._send_parameter_feedback(control_index, device_param, send_cc=False, force_display=True)

    def _send_parameter_display_value(self, control_index, device_param, force=False, throttle=False):
        if not device_param or not liveobj_valid(device_param):
            return
        if not self._is_current_parameter_for_control(control_index, device_param):
            return

        now = time.time()
        if throttle and not force:
            last_sent_at = self._last_parameter_display_feedback_times.get(control_index, 0.0)
            if now - last_sent_at < self.PARAMETER_DISPLAY_FEEDBACK_INTERVAL:
                return

        display_value = self._parameter_display_value(device_param)
        normalized_value = round(self._parameter_normalized_value(device_param), 9)

        if (not force and
            self._last_sent_parameter_displays.get(control_index) == display_value and
            self._last_sent_parameter_normalized_values.get(control_index) == normalized_value):
            return

        payload = "{}|{}|{}".format(
            control_index,
            normalized_value,
            self._escape_sysex_string(display_value)
        )
        self._send_sys_ex_message(payload, 0x28)
        self._last_sent_parameter_displays[control_index] = display_value
        self._last_sent_parameter_normalized_values[control_index] = normalized_value
        self._last_parameter_display_feedback_times[control_index] = now

    def _send_parameter_feedback(self, control_index, device_param, send_cc=False, force_display=False, throttle_display=False):
        if not device_param or not liveobj_valid(device_param):
            return
        if not self._is_current_parameter_for_control(control_index, device_param):
            return

        if send_cc:
            cc_value = self._parameter_value_to_cc(device_param)
            if self._last_sent_parameter_cc_values.get(control_index) != cc_value:
                self.send_cc(72 + control_index, 8, cc_value)
                self._last_sent_parameter_cc_values[control_index] = cc_value

        self._send_parameter_display_value(
            control_index,
            device_param,
            force=force_display,
            throttle=throttle_display
        )

    def _select_parameter_if_possible(self, mapped_parameter):
        try:
            if mapped_parameter and liveobj_valid(mapped_parameter) and hasattr(self.song().view, 'selected_parameter'):
                self.song().view.selected_parameter = mapped_parameter
        except Exception:
            pass

    def _arm_remove_automation_from_next_encoder(self, value):
        if value:
            self._remove_automation_from_next_encoder = True
            self._automation_parameter_action = "remove"
            self._automation_removal_suppressed_controls.clear()

    def _arm_re_enable_automation_from_next_encoder(self, value):
        if value:
            self._remove_automation_from_next_encoder = False
            self._automation_parameter_action = "re_enable"
            self._automation_removal_suppressed_controls.clear()

    def _pending_parameter_automation_action(self):
        if self._automation_parameter_action:
            return self._automation_parameter_action
        return "remove" if self._remove_automation_from_next_encoder else None

    def _consume_remove_automation_request(self, device_param, track=None):
        action = self._pending_parameter_automation_action()
        if not action:
            return False

        self._remove_automation_from_next_encoder = False
        self._automation_parameter_action = None
        if not device_param or not liveobj_valid(device_param):
            return True

        self._select_parameter_if_possible(device_param)
        if action == "re_enable":
            self._re_enable_automation_for_parameter(device_param)
        else:
            self._remove_automation_for_parameter(device_param, track)
        return True

    def _playing_clip_slot_for_track(self, track):
        try:
            if track is None or not hasattr(track, 'clip_slots'):
                return None
            for clip_slot in track.clip_slots:
                if clip_slot is not None and clip_slot.has_clip and clip_slot.is_playing:
                    return clip_slot
        except Exception:
            pass
        return None

    def _remove_automation_for_parameter(self, device_param, track=None):
        try:
            song = self.song()
            clip_slot = self._playing_clip_slot_for_track(track or song.view.selected_track)
            envelope_was_cleared = False
            if clip_slot is not None and clip_slot.has_clip:
                clip = clip_slot.clip
                envelope = None
                if hasattr(clip, 'automation_envelope'):
                    try:
                        envelope = clip.automation_envelope(device_param)
                    except Exception:
                        envelope = None
                if envelope is not None:
                    self._clear_clip_automation_envelope(clip, envelope, device_param, self._parameter_normalized_value(device_param))
                    self._clear_authored_automation_steps_for_parameter(clip, device_param)
                    envelope_was_cleared = True

            if envelope_was_cleared:
                self._refresh_parameter_metadata_on_automation_change()
        except Exception as e:
            self._debug_log("Error removing automation: {}".format(str(e)))

    def _automation_clear_end_time(self, clip):
        try:
            end_time = max(
                float(getattr(clip, "loop_end", 0.0)),
                float(getattr(clip, "end_marker", 0.0)),
                float(getattr(clip, "length", 0.0)),
                float(getattr(clip, "start_marker", 0.0)),
                float(getattr(clip, "loop_start", 0.0))
            )
        except Exception:
            end_time = 0.0

        info = self._decoupled_automation_info(clip)
        if info:
            try:
                end_time = max(end_time, float(info.get("physical_end", 0.0)))
            except Exception:
                pass

        return max(0.0001, end_time)

    def _clear_clip_automation_envelope(self, clip, envelope, device_param, normalized_value):
        if clip is None or envelope is None or device_param is None:
            return False

        if hasattr(clip, "clear_envelope"):
            try:
                clip.clear_envelope(device_param)
                return True
            except Exception:
                pass

        try:
            raw_value = self._parameter_target_value_from_normalized(device_param, normalized_value)
            envelope.insert_step(0.0, self._automation_clear_end_time(clip), raw_value)
            return True
        except Exception:
            pass

        return False

    def _neutralize_automation_span(self, envelope, device_param, start_time, end_time, normalized_value):
        if envelope is None or device_param is None:
            return False

        minimum_duration = 0.0001
        try:
            start_time = max(0.0, float(start_time))
            end_time = max(start_time + minimum_duration, float(end_time))
            raw_value = self._parameter_target_value_from_normalized(device_param, normalized_value)
            envelope.insert_step(start_time, max(minimum_duration, end_time - start_time), raw_value)
            return True
        except Exception:
            pass

        return False

    def _re_enable_automation_for_parameter(self, device_param):
        try:
            if hasattr(device_param, 're_enable_automation'):
                device_param.re_enable_automation()
                self._refresh_parameter_metadata_on_automation_change()
                self._send_re_enable_automation_enabled(force=True)
        except Exception as e:
            self._debug_log("Error re-enabling parameter automation: {}".format(str(e)))

    def _automation_envelope_key(self, clip, device_param, control_index):
        if clip is None or device_param is None:
            return None

        try:
            clip_identity = self._live_object_identity(clip)
        except Exception:
            clip_identity = id(clip)

        try:
            parameter_identity = self._live_object_identity(device_param)
        except Exception:
            parameter_identity = id(device_param)

        return (clip_identity, parameter_identity, int(control_index))

    def _authored_automation_steps(self, clip, device_param, control_index):
        key = self._automation_envelope_key(clip, device_param, control_index)
        if key is None:
            return None
        return self._automation_authored_steps.get(key)

    def _store_authored_automation_steps(self, clip, device_param, control_index, steps):
        key = self._automation_envelope_key(clip, device_param, control_index)
        if key is None:
            return
        self._automation_authored_steps[key] = self._automation_sorted_steps(steps)

    def _automation_step_id(self, step, fallback_index=0):
        try:
            if len(step) >= 5:
                return max(0, int(step[4]))
        except Exception:
            pass
        return max(0, int(fallback_index) + 1)

    def _automation_step_order(self, step, fallback_index=0):
        try:
            if len(step) >= 6:
                return max(0, int(step[5]))
        except Exception:
            pass
        return self._automation_step_id(step, fallback_index)

    def _automation_step_tuple(self, step, fallback_index=0):
        return (
            float(step[0]),
            float(step[1]),
            max(0.0, min(1.0, float(step[2]))),
            max(-1.0, min(1.0, float(step[3]) if len(step) >= 4 else 0.0)),
            self._automation_step_id(step, fallback_index),
            self._automation_step_order(step, fallback_index)
        )

    def _automation_sort_key(self, indexed_step):
        index, step = indexed_step
        return (float(step[0]), self._automation_step_order(step, index), self._automation_step_id(step, index), index)

    def _automation_sorted_steps(self, steps):
        return tuple(
            self._automation_step_tuple(step, index)
            for index, step in sorted(enumerate(tuple(steps or ())), key=self._automation_sort_key)
        )

    def _automation_step_entry(self, step):
        normalized_step = self._automation_step_tuple(step)
        time_value, duration, normalized, curve, step_id, step_order = normalized_step
        return "{:.6f}:{:.6f}:{:.6f}:{:.6f}:{}:{}".format(
            time_value,
            duration,
            normalized,
            curve,
            step_id,
            step_order
        )

    def _automation_payload_checksum(self, value):
        checksum = 0
        try:
            data = value.encode('ascii', errors='ignore')
        except Exception:
            data = bytes()
        for byte in data:
            checksum = ((checksum * 31) + byte) & 0x7fffffff
        return checksum

    def _clear_authored_automation_steps(self, clip, device_param, control_index):
        key = self._automation_envelope_key(clip, device_param, control_index)
        if key is None:
            return
        self._automation_authored_steps.pop(key, None)

    def _clear_authored_automation_steps_for_parameter(self, clip, device_param):
        if clip is None or device_param is None:
            return

        try:
            clip_identity = self._live_object_identity(clip)
            parameter_identity = self._live_object_identity(device_param)
        except Exception:
            return

        for key in list(self._automation_authored_steps.keys()):
            if len(key) >= 2 and key[0] == clip_identity and key[1] == parameter_identity:
                self._automation_authored_steps.pop(key, None)

    def _clear_authored_automation_steps_for_clip(self, clip):
        if clip is None:
            return

        try:
            clip_identity = self._live_object_identity(clip)
        except Exception:
            return

        for key in list(self._automation_authored_steps.keys()):
            if len(key) >= 1 and key[0] == clip_identity:
                self._automation_authored_steps.pop(key, None)

    def _on_device_control_value(self, value, control):
        try:
            control_index = self._device_controls.index(control) if hasattr(self, '_device_controls') and control in self._device_controls else -1
            if control_index >= 0 and self._set_track_midi_control_value_for_control(control_index, value=value):
                return

            mapped_parameter = self._current_connected_parameter_for_control(control_index) if control_index >= 0 else None
            self._remove_parameter_from_smooth_macro_randomize(mapped_parameter)
            if self._consume_remove_automation_request(mapped_parameter):
                return
            self._select_parameter_if_possible(mapped_parameter)
        except Exception as e:
            self._debug_log("Error selecting touched parameter: {}".format(str(e)))

    def _begin_high_resolution_undo_step(self, control_index):
        if control_index in self._active_high_resolution_undo_steps:
            return
        try:
            song = self.song()
            if hasattr(song, 'begin_undo_step'):
                song.begin_undo_step()
                self._active_high_resolution_undo_steps.add(control_index)
        except Exception as e:
            self._debug_log("Error beginning high resolution undo step: {}".format(str(e)))

    def _end_high_resolution_undo_step(self, control_index):
        if control_index not in self._active_high_resolution_undo_steps:
            return
        try:
            song = self.song()
            if hasattr(song, 'end_undo_step'):
                song.end_undo_step()
        except Exception as e:
            self._debug_log("Error ending high resolution undo step: {}".format(str(e)))
        self._active_high_resolution_undo_steps.discard(control_index)

    def _set_device_control_high_resolution(self, message):
        try:
            if len(message) < 7:
                return

            control_index = int(message[2])
            gesture_state = int(message[3])
            raw_value = ((int(message[4]) & 0x7F) << 14) | ((int(message[5]) & 0x7F) << 7) | (int(message[6]) & 0x7F)
            raw_value = max(0, min(65535, raw_value))

            if gesture_state == 4:
                track = self.song().view.selected_track
                kind = {0: "mod_wheel", 1: "pressure"}.get(control_index)
                entry = self._track_midi_control_entry_for_kind(kind, track)
                if entry:
                    self._set_track_midi_control_normalized_value(
                        entry,
                        float(raw_value) / 65535.0,
                        track
                    )
                return

            if self._track_device_is_selected():
                entry = self._track_device_parameter_entry_for_control(control_index)
                if entry and entry.get("kind") != "parameter":
                    if gesture_state in (0, 1, 2):
                        self._set_track_midi_control_value_for_control(
                            control_index,
                            normalized=float(raw_value) / 65535.0
                        )
                    elif gesture_state == 3:
                        self._set_track_midi_control_value_for_control(
                            control_index,
                            normalized=self._track_midi_control_default_normalized(entry)
                        )
                    return

            mapped_parameter = self._current_connected_parameter_for_control(control_index)
            if not mapped_parameter:
                if gesture_state == 2:
                    self._end_high_resolution_undo_step(control_index)
                    self._active_high_resolution_gestures.discard(control_index)
                return

            if control_index in self._automation_removal_suppressed_controls:
                if gesture_state == 2:
                    self._automation_removal_suppressed_controls.discard(control_index)
                    self._end_high_resolution_undo_step(control_index)
                    self._active_high_resolution_gestures.discard(control_index)
                return

            if self._consume_remove_automation_request(mapped_parameter):
                self._automation_removal_suppressed_controls.add(control_index)
                return

            if hasattr(mapped_parameter, 'is_enabled') and not mapped_parameter.is_enabled:
                if gesture_state == 2:
                    self._end_high_resolution_undo_step(control_index)
                    self._active_high_resolution_gestures.discard(control_index)
                return

            self._select_parameter_if_possible(mapped_parameter)

            if gesture_state == 3:
                self._set_device_control_next_option(control_index, mapped_parameter)
                return

            if gesture_state == 1:
                self._send_parameter_feedback(control_index, mapped_parameter, send_cc=False, force_display=True)
                return

            if gesture_state == 0:
                self._remove_parameter_from_smooth_macro_randomize(mapped_parameter)
                
                if control_index not in self._active_high_resolution_gestures:
                    self._begin_high_resolution_undo_step(control_index)
                    if hasattr(mapped_parameter, 'begin_gesture'):
                        mapped_parameter.begin_gesture()
                    self._active_high_resolution_gestures.add(control_index)

                if mapped_parameter.max == mapped_parameter.min:
                    return

                normalized = float(raw_value) / 65535.0
                target_value = self._parameter_target_value_from_normalized(mapped_parameter, normalized)
                mapped_parameter.value = target_value
                self._send_parameter_feedback(control_index, mapped_parameter, send_cc=False, throttle_display=True)

            if gesture_state == 2 and control_index in self._active_high_resolution_gestures:
                if hasattr(mapped_parameter, 'end_gesture'):
                    mapped_parameter.end_gesture()
                self._active_high_resolution_gestures.discard(control_index)
                self._end_high_resolution_undo_step(control_index)
                self._send_parameter_feedback(control_index, mapped_parameter, send_cc=False, force_display=True)
        except Exception as e:
            self._debug_log("Error setting high resolution parameter: {}".format(str(e)))

    def _bank_select(self, value):
        offset = value - 64
        if offset == 0:
            return

        if self._track_device_is_selected():
            all_bank_names = self._track_device_bank_names()
            if not all_bank_names:
                return

            current_index = max(0, min(len(all_bank_names) - 1, self._track_control_bank_index()))
            new_index = max(0, min(len(all_bank_names) - 1, current_index + offset))
            if new_index != current_index:
                self._set_track_control_bank_index(new_index)
                self._connect_track_device_parameter_controls()
                try:
                    self.request_rebuild_midi_map()
                except Exception:
                    pass
                self._on_device_changed(False)
            return

        if not liveobj_valid(self._device):
            return
        
        all_bank_names = self._device._parameter_bank_names()
        if not all_bank_names:
            return
        
        current_index = self._device._bank_index
        new_index = max(0, min(len(all_bank_names) - 1, current_index + offset))
        
        if new_index != current_index:
            self._device._bank_index = new_index
            self._device.update()
            self._on_device_changed(False)
            self._schedule_bank_metadata_refreshes()

    def _find_drum_rack_in_track(self, track):
        for device in track.devices:
            if device.can_have_drum_pads:
                return device
            elif isinstance(device, Live.RackDevice.RackDevice):
                # If the device is a RackDevice (e.g., Instrument Rack), recursively search inside its chains
                for chain in device.chains:
                    drum_rack = self._find_drum_rack_in_track(chain)
                    if drum_rack is not None:
                        return drum_rack
        return None
    
    def _setup_drum_pad_listeners(self):
        if self._drum_rack_device:
            self._send_all_drum_pad_names()
            self._send_selected_drum_pad_number()
            for pad in self._drum_rack_device.drum_pads:
                if not pad.name_has_listener(self._send_all_drum_pad_names):
                    pad.add_name_listener(self._send_all_drum_pad_names)
            if not self._drum_rack_device.view.selected_drum_pad_has_listener(self._send_selected_drum_pad_number):
                self._drum_rack_device.view.add_selected_drum_pad_listener(self._send_selected_drum_pad_number)

    def _remove_drum_pad_name_listeners(self):
        if self._drum_rack_device:
            for pad in self._drum_rack_device.drum_pads:
                if pad.name_has_listener(self._send_all_drum_pad_names):
                    pad.remove_name_listener(self._send_all_drum_pad_names)
            if self._drum_rack_device.view.selected_drum_pad_has_listener(self._send_selected_drum_pad_number):
                self._drum_rack_device.view.remove_selected_drum_pad_listener(self._send_selected_drum_pad_number)

    def _send_all_drum_pad_names(self):
        if not self._drum_rack_device:
            return
    
        pads = self._drum_rack_device.drum_pads
    
        # Find the first and last pad with chains using the .note property
        first_with_chain = None
        last_with_chain = None
    
        for pad in pads:
            if pad.chains:
                first_with_chain = pad
                break
    
        for pad in reversed(pads):
            if pad.chains:
                last_with_chain = pad
                break
                
        if first_with_chain and last_with_chain:
            first_index = first_with_chain.note
            last_index = last_with_chain.note
        
            pad_names = []
            
            # Add index of first chain pad
            pad_names.append(str(first_index))
            for pad in pads[first_index:last_index + 1]:
                if pad.chains:
                    pad_names.append(self._escape_sysex_string(pad.name))
                else:
                    pad_names.append(str(pad.note))
            payload = ",".join(pad_names)
            self._send_sys_ex_message(payload, 0x11)
    
    def _update_tempo(self):
        new_tempo = round(self.song().tempo, 2)
        self._send_sys_ex_message(str(new_tempo), 0x12)

    def _metronome_value(self):
        try:
            return bool(self.song().metronome)
        except Exception:
            return False

    def _update_metronome(self):
        self._send_sys_ex_message("1" if self._metronome_value() else "0", 0x17)
    
    def _is_bank_connected(self, device, bank_name):
        if not isinstance(device, Live.RackDevice.RackDevice):
            return True
        
        if bank_name not in ("Macros", "Macros 2"):
            return True
        
        if not hasattr(device, 'macros_mapped'):
            return True
        
        macro_ranges = {
            "Macros": (0, 8),
            "Macros 2": (8, 16)
        }
        
        if bank_name not in macro_ranges:
            return True
        
        start_idx, end_idx = macro_ranges[bank_name]
        return any(device.macros_mapped[start_idx:end_idx])

    def _app_device_index_to_live_index(self, value):
        try:
            return int(value) - 1
        except Exception:
            return -1

    def _track_control_key(self, track=None):
        try:
            track = track or self.song().view.selected_track
            return self._live_object_identity(track) if track else None
        except Exception:
            return None

    def _track_device_is_selected(self, track=None):
        key = self._track_control_key(track)
        if key is None:
            return False
        selections = getattr(self, "_track_control_selection_by_track", {})
        if key in selections:
            return bool(selections[key])
        try:
            track = track or self.song().view.selected_track
            has_devices = track and hasattr(track, "devices") and len(track.devices) > 0
            return not has_devices
        except Exception:
            return False

    def _set_track_device_selected(self, selected, track=None):
        key = self._track_control_key(track)
        if key is not None:
            self._track_control_selection_by_track[key] = bool(selected)
        self._track_device_selected = bool(selected)

    def _track_control_bank_index(self, track=None):
        key = self._track_control_key(track)
        if key is None:
            return 0
        return int(getattr(self, "_track_control_bank_index_by_track", {}).get(key, 0))

    def _set_track_control_bank_index(self, index, track=None):
        key = self._track_control_key(track)
        if key is not None:
            self._track_control_bank_index_by_track[key] = int(index)
        self._track_device_bank_index = int(index)

    def _send_name_for_index(self, index):
        if index < 0:
            return "Send"
        if index < 26:
            return "Send {}".format(chr(ord("A") + index))
        return "Send {}".format(index + 1)

    def _track_device_parameters(self, track=None):
        try:
            track = track or self.song().view.selected_track
    
            if track is None or not hasattr(track, "mixer_device"):
                return []
    
            mixer_device = track.mixer_device
    
            track_has_midi_input = bool(getattr(track, "has_midi_input", False))
            midi_controls = [
                dict(control)
                for control in self.TRACK_DEVICE_MIDI_CONTROLS
            ] if track_has_midi_input else []
    
            mixer_controls = [
                {
                    "name": "Volume",
                    "parameter": mixer_device.volume,
                    "kind": "parameter",
                    "automatable": True,
                },
                {
                    "name": "Pan",
                    "parameter": mixer_device.panning,
                    "kind": "parameter",
                    "automatable": True,
                },
            ]
    
            sends = [
                {
                    "name": self._send_name_for_index(send_index),
                    "parameter": send,
                    "kind": "parameter",
                    "automatable": True,
                }
                for send_index, send in enumerate(mixer_device.sends)
                if send and liveobj_valid(send)
            ]
    
            entries = midi_controls[:4] + mixer_controls + sends
    
            return [
                entry
                for entry in entries
                if entry.get("kind") != "parameter"
                or (
                    entry.get("parameter")
                    and liveobj_valid(entry.get("parameter"))
                )
            ]
    
        except Exception:
            return []

    def _track_device_main_entries(self, track=None):
        return self._track_device_parameters(track)[:self.TRACK_DEVICE_BANK_SIZE]

    def _track_device_extra_send_entries(self, track=None):
        return [
            entry for entry in self._track_device_parameters(track)[self.TRACK_DEVICE_BANK_SIZE:]
            if entry.get("name", "").startswith("Send ")
        ]

    def _track_device_bank_names(self, track=None):
        extra_sends = self._track_device_extra_send_entries(track)
        names = [self.TRACK_DEVICE_MAIN_BANK_NAME]
        if not extra_sends:
            return names

        send_bank_count = int(math.ceil(float(len(extra_sends)) / float(self.TRACK_DEVICE_BANK_SIZE)))
        if send_bank_count == 1:
            names.append(self.TRACK_DEVICE_SEND_BANK_NAME)
        else:
            for bank_index in range(send_bank_count):
                names.append("{} {}".format(self.TRACK_DEVICE_SEND_BANK_NAME, bank_index + 1))
        return names

    def _track_device_current_bank_index(self, track=None):
        bank_names = self._track_device_bank_names(track)
        if not bank_names:
            self._set_track_control_bank_index(0, track)
            return 0

        index = max(0, min(len(bank_names) - 1, self._track_control_bank_index(track)))
        self._set_track_control_bank_index(index, track)
        return index

    def _track_device_parameter_entry_for_control(self, control_index, track=None):
        bank_index = self._track_device_current_bank_index(track)
        if bank_index == 0:
            entries = self._track_device_main_entries(track)
        else:
            start = (bank_index - 1) * self.TRACK_DEVICE_BANK_SIZE
            entries = self._track_device_extra_send_entries(track)[start:start + self.TRACK_DEVICE_BANK_SIZE]
        if control_index >= 0 and control_index < len(entries):
            return entries[control_index]
        return None

    def _track_device_parameter_for_control(self, control_index, track=None):
        entry = self._track_device_parameter_entry_for_control(control_index, track)
        if entry and entry.get("kind") == "parameter":
            return entry.get("parameter")
        return None

    def _track_midi_control_state(self, track=None):
        key = self._track_control_key(track) or "global"
        state_by_track = getattr(self, "_track_midi_control_values_by_track", {})
        if key not in state_by_track:
            state_by_track[key] = {}
            self._track_midi_control_values_by_track = state_by_track
        return state_by_track[key]

    def _track_midi_control_default_normalized(self, entry):
        try:
            minimum = float(entry.get("min", 0.0))
            maximum = float(entry.get("max", 127.0))
            default = float(entry.get("default", minimum))
            if maximum == minimum:
                return 0.0
            return max(0.0, min(1.0, (default - minimum) / (maximum - minimum)))
        except Exception:
            return 0.0

    def _track_midi_control_normalized_value(self, entry, track=None):
        if not entry:
            return 0.0
        kind = entry.get("kind", "")
        return max(0.0, min(1.0, float(self._track_midi_control_state(track).get(kind, self._track_midi_control_default_normalized(entry)))))

    def _track_midi_control_entry_for_kind(self, kind, track=None):
        if not kind:
            return None
        try:
            track = track or self.song().view.selected_track
            if not bool(getattr(track, "has_midi_input", False)):
                return None
            for control in self.TRACK_DEVICE_MIDI_CONTROLS:
                if control.get("kind") == kind:
                    return dict(control)
        except Exception:
            pass
        return None

    def _set_track_midi_control_normalized_value(self, entry, normalized, track=None):
        if not entry:
            return
        kind = entry.get("kind", "")
        if not kind:
            return
        self._track_midi_control_state(track)[kind] = max(0.0, min(1.0, float(normalized)))

    def _track_midi_control_raw_value(self, entry, normalized):
        try:
            minimum = float(entry.get("min", 0.0))
            maximum = float(entry.get("max", 127.0))
            normalized = max(0.0, min(1.0, float(normalized)))
            return minimum + ((maximum - minimum) * normalized)
        except Exception:
            return 0.0

    def _track_midi_control_display_value(self, entry, normalized):
        raw_value = self._track_midi_control_raw_value(entry, normalized)
        try:
            if entry.get("kind") == "pitch_bend":
                offset = raw_value - self.TRACK_DEVICE_PITCH_BEND_CENTER
                denominator = (self.TRACK_DEVICE_PITCH_BEND_MAX - self.TRACK_DEVICE_PITCH_BEND_CENTER) if offset >= 0 else self.TRACK_DEVICE_PITCH_BEND_CENTER
                percent = int(round((offset / denominator) * 100.0)) if denominator else 0
                if percent == 0:
                    return "0"
                return "+{}".format(percent) if percent > 0 else str(percent)
            if entry.get("kind") == "velocity":
                return str(max(1, min(127, int(round(raw_value)))))
            return str(int(round(raw_value)))
        except Exception:
            return "0"

    def _send_track_midi_control_feedback(self, control_index, entry=None, normalized=None, track=None):
        entry = entry or self._track_device_parameter_entry_for_control(control_index, track)
        if not entry or entry.get("kind") == "parameter":
            return

        if normalized is None:
            normalized = self._track_midi_control_normalized_value(entry, track)
        normalized = max(0.0, min(1.0, float(normalized)))
        cc_value = max(0, min(127, int(round(normalized * 127.0))))
        display_value = self._track_midi_control_display_value(entry, normalized)

        self.send_cc(72 + control_index, 8, cc_value)
        self._last_sent_parameter_cc_values[control_index] = cc_value
        self._send_sys_ex_message(
            "{}|{}|{}".format(control_index, normalized, self._escape_sysex_string(display_value)),
            0x28
        )

    def _send_track_local_control_state(self, track=None):
        try:
            track = track or self.song().view.selected_track
            has_midi_input = bool(getattr(track, "has_midi_input", False))
            state = self._track_midi_control_state(track) if has_midi_input else {}
            mod_wheel = max(0.0, min(1.0, float(state.get("mod_wheel", 0.0))))
            pressure = max(0.0, min(1.0, float(state.get("pressure", 0.0))))
            payload = "mod_wheel|{:.6f},pressure|{:.6f}".format(mod_wheel, pressure)
            self._send_sys_ex_message(payload, 0x3B)
        except Exception:
            pass

    def _set_track_midi_control_value_for_control(self, control_index, value=None, normalized=None, track=None, require_selected=True, send_feedback=True):
        if require_selected and not self._track_device_is_selected(track):
            return False

        entry = self._track_device_parameter_entry_for_control(control_index, track)
        if not entry or entry.get("kind") == "parameter":
            return False

        if normalized is None:
            normalized = max(0.0, min(1.0, float(value) / 127.0))
        normalized = max(0.0, min(1.0, float(normalized)))
        self._set_track_midi_control_normalized_value(entry, normalized, track)
        if send_feedback:
            self._send_track_midi_control_feedback(control_index, entry, normalized, track)
        return True

    def _track_device_parameter_metadata_item(self, control_index, track=None):
        entry = self._track_device_parameter_entry_for_control(control_index, track)
        if not entry:
            return self.UNMAPPED_PARAMETER_METADATA_ITEM

        if entry.get("kind") != "parameter":
            name = self._escape_sysex_string(entry.get("name", ""))
            minimum = float(entry.get("min", 0.0))
            maximum = float(entry.get("max", 127.0))
            default = float(entry.get("default", minimum))
            normalized = self._track_midi_control_normalized_value(entry, track)
            display_value = self._track_midi_control_display_value(entry, normalized)
            fields = [
                name,
                str(int(minimum)) if minimum == int(minimum) else str(minimum),
                str(int(maximum)) if maximum == int(maximum) else str(maximum),
                str(int(default)) if default == int(default) else str(default),
                str(max(0.0, min(1.0, round(self._track_midi_control_default_normalized(entry), 3)))),
                str(int(minimum + ((maximum - minimum) * 32 / 127))),
                "",
                display_value,
                str(max(0.0, min(1.0, normalized))),
                self._escape_sysex_string(entry.get("kind", "")),
                "1" if entry.get("automatable", False) else "0",
            ]
            return "|".join(fields)

        fallback_name = entry.get("name", "")
        parameter = entry.get("parameter")
        name = self._get_parameter_display_name(parameter)
        try:
            escaped_parameter_name = self._escape_sysex_string(getattr(parameter, "name", ""))
            prefix = ""
            display_name = name
            for candidate_prefix in ("**", "*/", "*-"):
                if display_name.startswith(candidate_prefix):
                    prefix = candidate_prefix
                    display_name = display_name[len(candidate_prefix):]
                    break
            if display_name == escaped_parameter_name:
                name = prefix + self._escape_sysex_string(fallback_name)
        except Exception:
            pass

        min_val_str = None
        max_val_str = None
        default_val_str = None
        try:
            if hasattr(parameter, 'str_for_value') and hasattr(parameter, 'min') and hasattr(parameter, 'max'):
                min_val_str = parameter.str_for_value(parameter.min)
                max_val_str = parameter.str_for_value(parameter.max)
        except Exception:
            pass

        if min_val_str is None:
            min_val_str = str(parameter.min) if hasattr(parameter, 'min') else "0.0"
        if max_val_str is None:
            max_val_str = str(parameter.max) if hasattr(parameter, 'max') else "1.0"

        raw_default_value = 0.0
        quarter_str = "0.0"
        try:
            raw_default_value = getattr(parameter, "default_value", parameter.min if hasattr(parameter, "min") else 0.0)
            if hasattr(parameter, 'str_for_value'):
                default_val_str = parameter.str_for_value(raw_default_value)
                if hasattr(parameter, 'min') and hasattr(parameter, 'max'):
                    quarter_value = parameter.min + (parameter.max - parameter.min) * 32 / 127
                    quarter_str = parameter.str_for_value(quarter_value)
            else:
                default_val_str = str(round(raw_default_value, 2))
            if hasattr(parameter, 'min') and hasattr(parameter, 'max') and parameter.max != parameter.min:
                raw_default_value = round((raw_default_value - parameter.min) / (parameter.max - parameter.min), 3)
        except Exception:
            default_val_str = min_val_str

        value_items = ''
        if hasattr(parameter, 'is_quantized') and parameter.is_quantized and hasattr(parameter, 'value_items'):
            value_items = ';'.join(self._escape_sysex_string(item) for item in parameter.value_items)

        fields = [
            name.strip(),
            self._escape_sysex_string(str(min_val_str).strip()),
            self._escape_sysex_string(str(max_val_str).strip()),
            self._escape_sysex_string(str(default_val_str).strip()),
            str(raw_default_value).strip(),
            self._escape_sysex_string(str(quarter_str).strip()),
            value_items.strip(),
            self._escape_sysex_string(self._parameter_display_value(parameter)),
            str(self._parameter_normalized_value(parameter)),
            "parameter",
            "1",
        ]
        return "|".join(fields)

    def _build_track_device_parameter_metadata(self, track=None):
        return ",".join(
            self._track_device_parameter_metadata_item(index, track)
            for index in range(8)
        )

    def _send_track_device_bank_state(self, track_has_drums):
        bank_names = self._track_device_bank_names()
        bank_index = self._track_device_current_bank_index()
        current_bank_name = bank_names[bank_index] if bank_names else self.TRACK_DEVICE_MAIN_BANK_NAME
        bank_name_drum = self._escape_sysex_string(current_bank_name) + ";" + str(track_has_drums)
        self._send_sys_ex_message(bank_name_drum, 0x6D)
        self._send_sys_ex_message(",".join(self._escape_sysex_string(name) for name in bank_names), 0x5D)
        return current_bank_name, tuple(bank_names), tuple(bank_names)

    def _send_track_device_parameter_metadata(self, track=None):
        metadata = self._build_track_device_parameter_metadata(track)
        self._send_sys_ex_message(metadata, 0x7D)
        for control_index in range(8):
            entry = self._track_device_parameter_entry_for_control(control_index, track)
            parameter = entry.get("parameter") if entry and entry.get("kind") == "parameter" else None
            if parameter and liveobj_valid(parameter):
                self._send_parameter_feedback(control_index, parameter, send_cc=True, force_display=True)
            elif entry:
                self._send_track_midi_control_feedback(control_index, entry, track=track)
            else:
                self.send_cc(72 + control_index, 8, 0)

    def _send_bank_state(self, selected_device, track_has_drums):
        current_bank_name = self._device._bank_name
        all_bank_names = self._device._parameter_bank_names()
        connected_bank_names = [name for name in all_bank_names if self._is_bank_connected(selected_device, name)]

        # Handle case where current bank was filtered out
        if current_bank_name and isinstance(selected_device, Live.RackDevice.RackDevice):
            if current_bank_name in all_bank_names and current_bank_name not in connected_bank_names:
                # Current bank was filtered, navigate to first connected bank
                if connected_bank_names:
                    # Simulate bank navigation to trigger device change with valid bank
                    self._device._bank_index = all_bank_names.index(connected_bank_names[0])
                    current_bank_name = connected_bank_names[0]
                else:
                    current_bank_name = ""

        bank_name_drum = self._escape_sysex_string(current_bank_name) + ";" + str(track_has_drums)
        bank_names_list = ','.join(self._escape_sysex_string(name) for name in connected_bank_names)

        self._send_sys_ex_message(bank_name_drum, 0x6D)
        self._send_sys_ex_message(bank_names_list, 0x5D)
        return current_bank_name, all_bank_names, connected_bank_names

    def _sanitize_sysex_text(self, value):
        sanitized_value = str(value)
        for symbol, replacement in self.SYSEX_TEXT_SYMBOL_REPLACEMENTS:
            sanitized_value = sanitized_value.replace(symbol, replacement)

        def remove_ascii_empty_bracket_group(match):
            inner_value = match.group(1)
            ascii_inner_value = inner_value.encode('ascii', errors='ignore').decode('ascii')
            if inner_value.strip() and not ascii_inner_value.strip():
                return ''
            return match.group(0)

        sanitized_value = self.SYSEX_ROUND_BRACKET_TEXT_RE.sub(remove_ascii_empty_bracket_group, sanitized_value)
        sanitized_value = self.SYSEX_SQUARE_BRACKET_TEXT_RE.sub(remove_ascii_empty_bracket_group, sanitized_value)
        sanitized_value = self.SYSEX_CURLY_BRACKET_TEXT_RE.sub(remove_ascii_empty_bracket_group, sanitized_value)
        sanitized_value = sanitized_value.encode('ascii', errors='ignore').decode('ascii')
        sanitized_value = self.SYSEX_MULTI_SPACE_RE.sub(' ', sanitized_value)
        return sanitized_value.strip()

    def _escape_sysex_string(self, value):
        escaped_value = self._sanitize_sysex_text(value)
        escaped_value = escaped_value.replace(
            self.SYSEX_STRING_ESCAPE_CHAR,
            self.SYSEX_STRING_ESCAPE_CHAR + self.SYSEX_STRING_ESCAPE_CHAR
        )
        for symbol in self.SYSEX_STRING_RESERVED_SYMBOLS:
            if symbol == self.SYSEX_STRING_ESCAPE_CHAR:
                continue
            escaped_value = escaped_value.replace(symbol, self.SYSEX_STRING_ESCAPE_CHAR + symbol)
        return escaped_value

    def _unescape_sysex_string(self, value):
        result = []
        escaping = False
        for char in str(value):
            if escaping:
                result.append(char)
                escaping = False
            elif char == self.SYSEX_STRING_ESCAPE_CHAR:
                escaping = True
            else:
                result.append(char)
        if escaping:
            result.append(self.SYSEX_STRING_ESCAPE_CHAR)
        return ''.join(result)

    def _split_escaped_sysex_fields(self, value, separator):
        fields = []
        field_chars = []
        escaping = False
        for char in str(value):
            if escaping:
                field_chars.append(self.SYSEX_STRING_ESCAPE_CHAR)
                field_chars.append(char)
                escaping = False
            elif char == self.SYSEX_STRING_ESCAPE_CHAR:
                escaping = True
            elif char == separator:
                fields.append(''.join(field_chars))
                field_chars = []
            else:
                field_chars.append(char)
        if escaping:
            field_chars.append(self.SYSEX_STRING_ESCAPE_CHAR)
        fields.append(''.join(field_chars))
        return fields

    def _get_parameter_display_name(self, device_param):
        raw_name = self._escape_sysex_string(device_param.name)
        if hasattr(device_param, 'is_enabled'):
            if hasattr(device_param, 'automation_state') and device_param.automation_state != 0:
                if device_param.automation_state == 1:
                    return f"**{raw_name}"
                elif device_param.automation_state == 2:
                    return f"*/{raw_name}"
            elif device_param.is_enabled:
                return raw_name
            else:
                return f"*-{raw_name}"
        return raw_name

    def _current_bank_parameter_for_control(self, selected_device, control_index):
        try:
            if not hasattr(self, '_device') or not liveobj_valid(self._device):
                return None

            _, bank_parameters = self._device._current_bank_details()
            if bank_parameters and control_index < len(bank_parameters):
                bank_param = bank_parameters[control_index]
                if bank_param and liveobj_valid(bank_param):
                    if selected_device and hasattr(selected_device, 'parameters'):
                        if not any(device_param == bank_param for device_param in selected_device.parameters):
                            return None
                    return bank_param
        except Exception:
            pass
        return None

    def _selected_device(self):
        try:
            selected_track = self.song().view.selected_track
            return selected_track.view.selected_device if selected_track else None
        except Exception:
            return None

    def _current_connected_parameter_for_control(self, control_index, selected_device=None):
        if self._track_device_is_selected():
            track_parameter = self._track_device_parameter_for_control(control_index)
            return track_parameter if track_parameter and liveobj_valid(track_parameter) else None

        selected_device = selected_device or self._selected_device()
        bank_param = self._current_bank_parameter_for_control(selected_device, control_index)
        if bank_param and liveobj_valid(bank_param):
            return bank_param

        mapped_param = self._mapped_parameter_for_device_control(control_index)
        if mapped_param and selected_device and hasattr(selected_device, 'parameters'):
            try:
                if not any(device_param == mapped_param for device_param in selected_device.parameters):
                    return None
            except Exception:
                return None
        return mapped_param

    def _is_current_parameter_for_control(self, control_index, device_param):
        if not device_param or not liveobj_valid(device_param):
            return False

        current_param = self._current_connected_parameter_for_control(control_index)
        return current_param and liveobj_valid(current_param) and current_param == device_param
    
    def _build_parameter_metadata(self, selected_device):
        if self._track_device_is_selected():
            return self._build_track_device_parameter_metadata()

        if not hasattr(selected_device, 'parameters'):
            return ""
        
        param_data = []
        device_parameters = list(selected_device.parameters)
        device_param_map = {}
        for dp in device_parameters:
            if hasattr(dp, 'name'):
                device_param_map[dp.name] = dp
        unmapped_encoder_indices = []
        
        for control_index in range(8):
            mapped_param = self._current_connected_parameter_for_control(control_index, selected_device)
            
            if mapped_param:
                device_param = mapped_param
                if device_param is None or not hasattr(device_param, 'name'):
                    device_param = device_param_map.get(mapped_param.name)
                
                if device_param:
                    name = self._get_parameter_display_name(device_param)
                    
                    min_val_str = None
                    max_val_str = None
                    default_val_str = None
                    
                    if min_val_str is None or max_val_str is None:
                        try:
                            if hasattr(device_param, 'str_for_value'):
                                if hasattr(device_param, 'min') and hasattr(device_param, 'max'):
                                    min_val_str = device_param.str_for_value(device_param.min)
                                    max_val_str = device_param.str_for_value(device_param.max)
                                else:
                                    min_val_str = device_param.str_for_value(0.0)
                                    max_val_str = device_param.str_for_value(1.0)
                        except Exception:
                            pass
                    
                    if min_val_str is None:
                        min_val_str = str(device_param.min) if hasattr(device_param, 'min') else "0.0"
                    if max_val_str is None:
                        max_val_str = str(device_param.max) if hasattr(device_param, 'max') else "1.0"
                    
                    raw_default_value = None
                    quarter_str = "0.0"
                    if (not device_param.is_quantized and hasattr(device_param, 'default_value')):
                        try:
                            raw_default_value = device_param.default_value
                            if hasattr(device_param, 'str_for_value'):
                                default_val_str = device_param.str_for_value(device_param.default_value)
                                try:
                                    num_val = float(default_val_str)
                                    default_val_str = str(round(num_val, 2))
                                except Exception:
                                    pass
                                if hasattr(device_param, 'min') and hasattr(device_param, 'max'):
                                    quarter_value = device_param.min + (device_param.max - device_param.min) * 32/127
                                    quarter_str = device_param.str_for_value(quarter_value)
                            else:
                                default_val_str = str(round(device_param.default_value, 2))
                            if hasattr(device_param, 'min') and hasattr(device_param, 'max') and device_param.max != device_param.min:
                                raw_default_value = round((raw_default_value - device_param.min) / (device_param.max - device_param.min), 3)
                        except Exception:
                            default_val_str = min_val_str
                            raw_default_value = device_param.min if hasattr(device_param, 'min') else 0.0
                    else:
                        default_val_str = min_val_str
                        raw_default_value = device_param.min if hasattr(device_param, 'min') else 0.0
                    
                    if hasattr(device_param, 'is_quantized') and device_param.is_quantized and hasattr(device_param, 'value_items'):
                        value_items = ';'.join(self._escape_sysex_string(item) for item in device_param.value_items)
                    else:
                        value_items = ''

                    default_raw_str = str(raw_default_value) if raw_default_value is not None else ""
                    min_val_str = self._escape_sysex_string(min_val_str.strip())
                    max_val_str = self._escape_sysex_string(max_val_str.strip())
                    default_val_str = self._escape_sysex_string(default_val_str.strip())
                    quarter_str = self._escape_sysex_string(quarter_str.strip())
                    param_str = f"{name.strip()}|{min_val_str}|{max_val_str}|{default_val_str}|{default_raw_str.strip()}|{quarter_str}|{value_items.strip()}"
                    param_data.append(param_str)
                else:
                    param_str = "*--&&-|0|127|0.0|0.0|32|"
                    param_data.append(param_str)
                    unmapped_encoder_indices.append(control_index)
            else:
                param_str = "*--&&-|0|127|0.0|0.0|32|"
                param_data.append(param_str)
                unmapped_encoder_indices.append(control_index)
        
        return ','.join(param_data)
    
    def _metadata_has_only_numbers(self, metadata):
        if not metadata:
            return False
        
        params = self._split_escaped_sysex_fields(metadata, ',')
        
        for param in params:
            fields = self._split_escaped_sysex_fields(param, '|')
            for field_index in [1, 2, 3, 5]:
                if field_index < len(fields):
                    field_value = self._unescape_sysex_string(fields[field_index].strip())
                    if field_value:
                        try:
                            float(field_value)
                        except ValueError:
                            return False
        
        return True

    def _metadata_has_raw_0_127(self, metadata):
        if not metadata:
            return False
        
        params = self._split_escaped_sysex_fields(metadata, ',')
        for param in params:
            fields = self._split_escaped_sysex_fields(param, '|')
            for field_index in [1, 2, 3, 5]:
                if field_index < len(fields):
                    field_value = self._unescape_sysex_string(fields[field_index].strip())
                    if field_value in ("0", "127"):
                        return True
        return False

    def _metadata_has_unmapped(self, metadata):
        if not metadata:
            return False
        
        return "*--&&-" in metadata

    def _metadata_is_all_unmapped(self, metadata):
        if not metadata:
            return False

        params = self._split_escaped_sysex_fields(metadata, ',')
        if len(params) < 8:
            return False

        for param in params[:8]:
            fields = self._split_escaped_sysex_fields(param, '|')
            if not fields or fields[0] != "*--&&-":
                return False
        return True

    def _send_unmapped_parameter_metadata(self):
        self._send_sys_ex_message(self.UNMAPPED_PARAMETER_METADATA, 0x7D)

        for control_index in range(8):
            cc_number = 72 + control_index
            self.send_cc(cc_number, 8, 0)

    def _send_unmapped_parameter_metadata_for_device(self, device, metadata=None):
        self._send_unmapped_parameter_metadata()
        self._set_cached_metadata(device, metadata or self.UNMAPPED_PARAMETER_METADATA)
        self._mark_metadata_sent(device)

    def _schedule_parameter_metadata_recheck(self, delay=None):
        if self._metadata_recheck_timer:
            self._metadata_recheck_timer.cancel()

        self._drum_pad_change_recheck_count = 0
        self._drum_pad_recheck_start = time.time()

        recheck_delay = delay if delay is not None else self.PARAMETER_METADATA_RECHECK_INTERVAL
        self._metadata_recheck_timer = threading.Timer(recheck_delay, self._recheck_parameter_metadata)
        self._metadata_recheck_timer.start()

    def _cancel_bank_metadata_refreshes(self):
        for timer in list(getattr(self, '_bank_metadata_refresh_timers', [])):
            try:
                timer.cancel()
            except Exception:
                pass
        self._bank_metadata_refresh_timers = []

    def _schedule_bank_metadata_refreshes(self):
        self._cancel_bank_metadata_refreshes()
        for delay in (0.05, 0.15, 0.35, 0.75):
            timer = threading.Timer(delay, self._refresh_active_bank_metadata)
            self._bank_metadata_refresh_timers.append(timer)
            timer.start()

    def _refresh_active_bank_metadata(self):
        if not liveobj_valid(self._device):
            return

        try:
            if hasattr(self._device, 'update'):
                self._device.update()
            self._force_send_current_bank_metadata()
        except Exception as e:
            self._debug_log("Error refreshing bank metadata after bank change: {}".format(str(e)))

    def _force_send_current_bank_metadata(self):
        selected_track = self.song().view.selected_track
        selected_device = selected_track.view.selected_device if selected_track else None
        if not selected_device or not hasattr(selected_device, 'parameters') or not selected_device.parameters:
            return

        current_metadata = self._build_parameter_metadata(selected_device)
        if not current_metadata:
            return

        if self._metadata_is_all_unmapped(current_metadata):
            if self._get_cached_metadata(selected_device) != current_metadata:
                self._send_unmapped_parameter_metadata_for_device(selected_device, current_metadata)
            return

        if self._get_cached_metadata(selected_device) == current_metadata:
            return

        self._send_sys_ex_message(current_metadata, 0x7D)
        self._set_cached_metadata(selected_device, current_metadata)
        self._mark_metadata_sent(selected_device)
        self._refresh_parameter_value_listeners_current_bank(send_current_values=True)
        self._refresh_parameter_name_listeners_current_bank()
        self._refresh_automation_state_listeners_current_bank()

    def _metadata_needs_rack_recheck(self, metadata):
        return (
            self._metadata_has_unmapped(metadata)
            or self._metadata_has_only_numbers(metadata)
            or self._metadata_has_raw_0_127(metadata)
        )

    def _debug_log(self, message):
        if self._debug_mode:
            self.log_message(message)

    def _get_device_cache_key(self, device):
        if not device:
            return None
        return id(device)

    def _get_cached_metadata(self, device):
        key = self._get_device_cache_key(device)
        if key is None:
            return None
        return self._metadata_cache.get(key)

    def _set_cached_metadata(self, device, metadata):
        key = self._get_device_cache_key(device)
        if key is None:
            return
        self._metadata_cache[key] = metadata

    def _clear_cached_metadata(self, device):
        key = self._get_device_cache_key(device)
        if key is None:
            return
        if key in self._metadata_cache:
            del self._metadata_cache[key]

    def _mark_metadata_sent(self, device):
        key = self._get_device_cache_key(device)
        self._metadata_send_seq += 1
        if key is not None:
            self._metadata_send_seq_by_device[key] = self._metadata_send_seq
    
    def _recheck_parameter_metadata(self):
        self._metadata_recheck_timer = None
        
        if not liveobj_valid(self._device):
            return
        
        selected_track = self.song().view.selected_track
        selected_device = selected_track.view.selected_device
        metadata_device = selected_device
        
        if self._drum_rack_device and selected_device == self._drum_rack_device:
            mapped_device = self._find_mapped_device()
            if mapped_device and self._is_device_in_drum_pad(mapped_device):
                metadata_device = mapped_device
        
        is_rack_device = isinstance(metadata_device, Live.RackDevice.RackDevice)
        is_drum_rack = self._drum_rack_device and metadata_device == self._drum_rack_device
        is_drum_pad_device = self._drum_rack_device and self._is_device_in_drum_pad(metadata_device)

        current_metadata = self._build_parameter_metadata(metadata_device)
        has_unmapped = self._metadata_has_unmapped(current_metadata)
        has_only_numbers = self._metadata_has_only_numbers(current_metadata)
        all_unmapped = self._metadata_is_all_unmapped(current_metadata)
        
        if self._drum_pad_recheck_start is None:
            self._drum_pad_recheck_start = time.time()
        
        elapsed = time.time() - self._drum_pad_recheck_start
        self._debug_log(
            "Recheck metadata: "
            f"iter={self._drum_pad_change_recheck_count} "
            f"elapsed={round(elapsed, 3)}s "
            f"device='{metadata_device.name if metadata_device else 'None'}' "
            f"rack={is_rack_device} drum_rack={bool(is_drum_rack)} drum_pad_device={bool(is_drum_pad_device)} "
            f"unmapped={has_unmapped} all_unmapped={all_unmapped} only_numbers={has_only_numbers}"
        )
        
        if elapsed >= self.PARAMETER_METADATA_RECHECK_DURATION:
            self._debug_log("Recheck metadata: reached max duration {}s, stopping".format(self.PARAMETER_METADATA_RECHECK_DURATION))
            self._drum_pad_change_recheck_count = 0
            self._drum_pad_recheck_start = None
            return
        
        should_resend = False
        cached_metadata = self._get_cached_metadata(metadata_device)
        if current_metadata and cached_metadata != current_metadata:
            should_resend = True
        
        if should_resend:
            if all_unmapped:
                self._debug_log(f"Recheck metadata (iteration {getattr(self, '_drum_pad_change_recheck_count', 0)}): Sending unmapped metadata")
                self._send_unmapped_parameter_metadata_for_device(metadata_device, current_metadata)
            else:
                self._debug_log(f"Recheck metadata (iteration {getattr(self, '_drum_pad_change_recheck_count', 0)}): Resending changed metadata")
                self._send_sys_ex_message(current_metadata, 0x7D)
                self._set_cached_metadata(metadata_device, current_metadata)
                self._mark_metadata_sent(metadata_device)
                self._refresh_parameter_value_listeners_current_bank(send_current_values=True)
                self._refresh_parameter_name_listeners_current_bank()
                self._refresh_automation_state_listeners_current_bank()
            
            if is_drum_pad_device:
                self._last_drum_pad_metadata = current_metadata
            else:
                self._last_sent_metadata = current_metadata
            
            if not all_unmapped and hasattr(metadata_device, 'parameters') and metadata_device.parameters:
                for control_index in range(8):
                    mapped_param = self._current_connected_parameter_for_control(control_index, metadata_device)
                    if mapped_param:
                        self._send_parameter_feedback(control_index, mapped_param, force_display=True)
        
        max_iterations = int(self.PARAMETER_METADATA_RECHECK_DURATION / self.PARAMETER_METADATA_RECHECK_INTERVAL) + 1
        should_continue = False
        if is_drum_pad_device or is_drum_rack:
            should_continue = has_unmapped
        elif is_rack_device:
            should_continue = has_only_numbers
        else:
            should_continue = all_unmapped
        
        if should_continue:
            self._drum_pad_change_recheck_count += 1
            if self._drum_pad_change_recheck_count <= max_iterations:
                self._metadata_recheck_timer = threading.Timer(self.PARAMETER_METADATA_RECHECK_INTERVAL, self._recheck_parameter_metadata)
                self._metadata_recheck_timer.start()
            else:
                if is_drum_pad_device:
                    self._debug_log(f"Drum pad recheck: Reached max iterations, last metadata: {current_metadata[:100]}...")
                    self._last_drum_pad_metadata = None
                else:
                    if is_drum_rack:
                        self._debug_log(f"Drum rack recheck: Reached max iterations, last metadata: {current_metadata[:100]}...")
                    else:
                        self._debug_log(f"Rack recheck: Reached max iterations, last metadata: {current_metadata[:100]}...")
                self._drum_pad_change_recheck_count = 0
                self._drum_pad_recheck_start = None
                return
        else:
            self._drum_pad_change_recheck_count = 0
            self._drum_pad_recheck_start = None
    
    @subject_slot('device')
    def _on_device_changed(self, send_device_navigation=True):
        if liveobj_valid(self._device):
            # get and send name of bank and device
            selected_track = self.song().view.selected_track
            selected_device = selected_track.view.selected_device if selected_track else None

            # Send the light bank update before listener and metadata work.
            track_has_drums = 0
            drum_rack_device = self._find_drum_rack_in_track(selected_track) if selected_track else None
            if drum_rack_device is not None:
                track_has_drums = 1

            if self._track_device_is_selected():
                self._connect_track_device_parameter_controls(selected_track)
                self._send_track_device_bank_state(track_has_drums)
                self._remove_parameter_value_listeners()
                self._remove_parameter_name_listeners()
                self._remove_parameter_source_listener()
                self._remove_automation_state_listeners()
                self._automation_metadata_device_id = None
                if selected_track and hasattr(selected_track, "mixer_device"):
                    self._set_parameter_source_listener(selected_track.mixer_device)

                available_devices_string = self._escape_sysex_string(self.TRACK_DEVICE_NAV_NAME)
                all_devices = []
                chain_info = []
                if send_device_navigation and selected_track and hasattr(selected_track, "devices"):
                    all_devices, chain_info = self._get_all_nested_devices(selected_track.devices)
                    all_device_names = [self._escape_sysex_string(self.TRACK_DEVICE_NAV_NAME)]
                    starts_by_index = {}
                    ends_by_index = {}
                    for info in chain_info:
                        starts_by_index.setdefault(info.get('start_index'), []).append(info)
                        ends_by_index.setdefault(info.get('end_index'), []).append(info)
                    for i, device in enumerate(all_devices):
                        name = device.name
                        starts = starts_by_index.get(i, ())
                        ends = ends_by_index.get(i, ())

                        prefix = ""
                        for s in starts:
                            if s['type'] == 'rack':
                                prefix += "||"
                            elif s['type'] == 'chain':
                                prefix += "|*"

                        suffix = ""
                        for e in ends:
                            if e['type'] == 'chain':
                                suffix += "*|"
                        for e in ends:
                            if e['type'] == 'rack':
                                suffix += "||"

                        all_device_names.append(prefix + self._escape_sysex_string(name) + suffix)
                    available_devices_string = ','.join(all_device_names)

                    self._send_sys_ex_message("0", 0x4D)
                    self._send_sys_ex_message(available_devices_string, 0x01)
                    self._send_rack_snapshot_state(all_devices)

                self._send_track_device_parameter_metadata(selected_track)
                self._refresh_parameter_value_listeners_current_bank(send_current_values=True)
                self._refresh_parameter_name_listeners_current_bank()
                self._refresh_automation_state_listeners_current_bank()
                self._last_automation_signature = self._get_automation_signature()
                return

            current_bank_name, all_bank_names, connected_bank_names = self._send_bank_state(selected_device, track_has_drums)

            self._remove_parameter_value_listeners()
            self._remove_parameter_name_listeners()
            self._remove_parameter_source_listener()
            self._remove_automation_state_listeners()
            self._automation_metadata_device_id = None
            self._set_parameter_source_listener(selected_device)
            
            selected_device_index = "not found"
            available_devices_string = ""
            if send_device_navigation:
                # Get all available devices of the selected track, including nested devices
                all_devices, chain_info = self._get_all_nested_devices(selected_track.devices)
                starts_by_index = {}
                ends_by_index = {}
                for info in chain_info:
                    starts_by_index.setdefault(info.get('start_index'), []).append(info)
                    ends_by_index.setdefault(info.get('end_index'), []).append(info)
                
                # Convert device objects to names for display, adding chain markers
                all_device_names = []
                for i, device in enumerate(all_devices):
                    name = device.name
                    
                    # collect all starts/ends for this index
                    starts = starts_by_index.get(i, ())
                    ends = ends_by_index.get(i, ())
                    
                    # there should never be more than one rack or chain at the same index
                    prefix = ""
                    for s in starts:
                        if s['type'] == 'rack':
                            prefix += "||"
                        elif s['type'] == 'chain':
                            prefix += "|*"
                
                    # make sure chains come first, racks after
                    suffix = ""
                    for e in ends:
                        if e['type'] == 'chain':
                            suffix += "*|"
                    for e in ends:
                        if e['type'] == 'rack':
                            suffix += "||"
                
                    all_device_names.append(prefix + self._escape_sysex_string(name) + suffix)
                
                available_devices_string = ','.join(all_device_names)
                if all_device_names:
                    available_devices_string = self._escape_sysex_string(self.TRACK_DEVICE_NAV_NAME) + "," + available_devices_string
                else:
                    available_devices_string = self._escape_sysex_string(self.TRACK_DEVICE_NAV_NAME)
                
                # CHANGE 2: Find index of selected device in our comprehensive nested devices list
                for index, device in enumerate(all_devices):
                    if device == selected_device:
                        selected_device_index = str(index + 1)
                        break
            
            # set up drum pad listeners after the fast bank update
            if drum_rack_device is not None:
                if drum_rack_device != self._drum_rack_device:
                    if self._drum_rack_device:
                        self._remove_drum_pad_name_listeners()
                    self._drum_rack_device = drum_rack_device
                    self._setup_drum_pad_listeners()
            elif self._drum_rack_device:
                self._remove_drum_pad_name_listeners()
                self._drum_rack_device = None
                
            if send_device_navigation:
                # CHANGE 3: Send the index from our comprehensive device list
                self._send_sys_ex_message(selected_device_index, 0x4D)
                
                # Send the comprehensive list of available devices
                self._send_sys_ex_message(available_devices_string, 0x01)
                self._send_rack_snapshot_state(all_devices)
            
            # In mixer mode, temporarily reconnect device controls so we can
            # build parameter metadata. Do this early so the framework has more
            # time to remap parameters to the new device before we read them.
            if self.mixer_status:
                self._connect_device_controls()
            
            if hasattr(selected_device, 'parameters') and selected_device.parameters:
                def _build_parameter_names():
                    names = []
                    for control_index, control in enumerate(self._device._parameter_controls):
                        mapped_param = self._current_connected_parameter_for_control(control_index, selected_device)
                        if mapped_param:
                            
                            device_param = mapped_param
                            
                            if device_param and hasattr(device_param, 'is_enabled'):
                                if hasattr(device_param, 'automation_state') and device_param.automation_state != 0:
                                    if device_param.automation_state == 1:
                                        names.append(f"**{self._escape_sysex_string(device_param.name)}")
                                    elif device_param.automation_state == 2:
                                        names.append(f"*/{self._escape_sysex_string(device_param.name)}")
                                elif device_param.is_enabled:
                                    names.append(self._escape_sysex_string(device_param.name))
                                else:
                                    names.append(f"*-{self._escape_sysex_string(device_param.name)}")
                            else:
                                # Fallback to mapped parameter name if DeviceParameter not found
                                names.append(self._escape_sysex_string(mapped_param.name))
                        else:
                            names.append("")
                    return [name for name in names if name != ""]
                
                parameter_names = _build_parameter_names()
                parameter_state_refreshed = False
                if not parameter_names and self.mixer_status:
                    if hasattr(self._device, 'set_device'):
                        try:
                            self._device.set_device(selected_device)
                        except Exception:
                            pass
                    if connected_bank_names:
                        try:
                            if current_bank_name and current_bank_name in all_bank_names:
                                self._device._bank_index = all_bank_names.index(current_bank_name)
                            else:
                                self._device._bank_index = all_bank_names.index(connected_bank_names[0])
                        except Exception:
                            pass
                    if hasattr(self._device, 'update'):
                        try:
                            self._device.update()
                        except Exception:
                            pass
                    parameter_names = _build_parameter_names()
                
                # Filter out empty names but keep "-" for disabled parameters
                
                if parameter_names:
                    parameter_state_refreshed = bool(self._send_parameter_info(parameter_names))
                else:
                    # parameter_names empty — try building metadata directly from
                    # _build_parameter_metadata, which may succeed even when the
                    # device component's own remapping is still in progress
                    current_metadata = self._build_parameter_metadata(selected_device)
                    if current_metadata and not self._metadata_is_all_unmapped(current_metadata):
                        self._send_sys_ex_message(current_metadata, 0x7D)
                        self._set_cached_metadata(selected_device, current_metadata)
                        self._mark_metadata_sent(selected_device)
                    else:
                        # Live sometimes needs another update tick after controls
                        # are reattached, so keep rechecking after sending the
                        # current empty state.
                        self._send_unmapped_parameter_metadata_for_device(selected_device, current_metadata)
                        self._schedule_parameter_metadata_recheck(delay=0.05)
                if not parameter_state_refreshed:
                    self._refresh_parameter_value_listeners_current_bank(send_current_values=True)
                    self._refresh_parameter_name_listeners_current_bank()
                    self._refresh_automation_state_listeners_current_bank()
                self._last_automation_signature = self._get_automation_signature()
            
            self._remove_disabled_parameter_listeners()
            
            if hasattr(selected_device, 'parameters') and selected_device.parameters:
                for control_index, control in enumerate(self._device._parameter_controls):
                    mapped_param = self._current_connected_parameter_for_control(control_index, selected_device)
                    if mapped_param:
                        
                        device_param = mapped_param
                        
                        if device_param and hasattr(device_param, 'is_enabled') and not device_param.is_enabled:
                            listener = self._create_disabled_param_listener(device_param, control_index)
                            if not device_param.value_has_listener(listener):
                                device_param.add_value_listener(listener)
                            
                            self._disabled_parameter_listeners[(device_param, control_index)] = listener
                            self._disabled_parameters.append(device_param)
                            self._current_disabled_controls.append(control_index)
                            
                            self._send_parameter_feedback(control_index, device_param, force_display=True)
            else:
                # Device has no parameters - send not mapped for all controls
                self._send_unmapped_parameter_metadata()

            # In mixer mode, disconnect device controls after a short delay
            # instead of immediately, so the framework has time to finish
            # remapping parameters to the new device.
            if self.mixer_status:
                if hasattr(self, '_mixer_disconnect_timer') and self._mixer_disconnect_timer:
                    self._mixer_disconnect_timer.cancel()
                self._mixer_disconnect_timer = threading.Timer(0.2, self._disconnect_device_controls)
                self._mixer_disconnect_timer.start()

        else:
            # no device
            # sending sysex of bank name, device name, bank names
            bank_name_drum = ";0"
            bank_names_list = ""
            available_devices_string = ""
            self._send_sys_ex_message(bank_name_drum, 0x6D)
            self._send_sys_ex_message(bank_names_list, 0x5D)
            self._send_sys_ex_message(available_devices_string, 0x01)
            self._send_rack_snapshot_state([])
            # Send not mapped for all controls when no device is selected
            self._send_unmapped_parameter_metadata()
    
    def _get_all_nested_devices(self, devices):
        """
        Recursively collect all devices, including those inside instruments and drum racks.
        For drum racks, only include the selected drum pad chain (like Push behavior).
        Returns a tuple: (list of device objects, list of chain_info dicts)
        
        chain_info format: {'start_index': int, 'end_index': int, 'type': 'drum_rack'/'rack'}
        """
        all_devices = []
        chain_info = []
        
        for device in devices:
            if liveobj_valid(device):
                # Add the current device
                all_devices.append(device)
                
                # Check if device is a drum rack
                if hasattr(device, 'can_have_drum_pads') and device.can_have_drum_pads and hasattr(device, 'drum_pads') and device.drum_pads:
                    # For drum racks, only process the selected drum pad chain
                    selected_drum_pad = self._get_selected_drum_pad(device)
                    if selected_drum_pad and hasattr(selected_drum_pad, 'chains') and selected_drum_pad.chains:
                        for chain in selected_drum_pad.chains:
                            if liveobj_valid(chain) and hasattr(chain, 'devices'):
                                # Mark start of nested chain
                                start_index = len(all_devices)
                                
                                # Add devices from the selected drum pad chain
                                nested_devices, nested_chain_info = self._get_all_nested_devices(chain.devices)
                                all_devices.extend(nested_devices)
                                
                                # Mark end of nested chain (if any devices were added)
                                if nested_devices:
                                    end_index = len(all_devices) - 1
                                    chain_info.append({
                                        'start_index': start_index,
                                        'end_index': end_index,
                                        'type': 'rack'
                                    })
                                
                                # Add any nested chain info with adjusted indices
                                for info in nested_chain_info:
                                    chain_info.append({
                                        'start_index': info['start_index'] + start_index,
                                        'end_index': info['end_index'] + start_index,
                                        'type': info['type']
                                    })
                
                # Check if device is an instrument rack or effect rack (has chains)
                elif hasattr(device, 'chains') and device.chains:
                    num_chains = len([c for c in device.chains if liveobj_valid(c) and hasattr(c, 'devices')])
                    
                    for chain in device.chains:
                        if liveobj_valid(chain) and hasattr(chain, 'devices'):
                            # Mark start of nested chain
                            start_index = len(all_devices)
                            
                            # Add devices from this chain
                            nested_devices, nested_chain_info = self._get_all_nested_devices(chain.devices)
                            all_devices.extend(nested_devices)
                            
                            # Mark end of nested chain (if any devices were added)
                            # adding chain instead of rack if we have more than one chain
                            if nested_devices:
                                end_index = len(all_devices) - 1
                                chain_info.append({
                                    'start_index': start_index,
                                    'end_index': end_index,
                                    'type': 'chain' if num_chains > 1 else 'rack'
                                })
                            
                            # Add any nested chain info with adjusted indices
                            for info in nested_chain_info:
                                chain_info.append({
                                    'start_index': info['start_index'] + start_index,
                                    'end_index': info['end_index'] + start_index,
                                    'type': info['type']
                                })
                            
        return all_devices, chain_info
    
    def _rack_snapshot_info_for_device(self, device):
        if not liveobj_valid(device) or not isinstance(device, Live.RackDevice.RackDevice):
            return None
        
        has_mapped_macros = False
        try:
            if hasattr(device, 'has_macro_mappings'):
                has_mapped_macros = bool(device.has_macro_mappings)
        except Exception:
            has_mapped_macros = False
        
        if not has_mapped_macros and hasattr(device, 'macros_mapped'):
            try:
                has_mapped_macros = any(device.macros_mapped)
            except Exception:
                has_mapped_macros = False
        
        variation_count = 0
        if hasattr(device, 'variation_count'):
            try:
                variation_count = int(device.variation_count)
            except Exception:
                variation_count = 0
        
        selected_variation_index = -1
        if variation_count > 0 and hasattr(device, 'selected_variation_index'):
            try:
                selected_variation_index = int(device.selected_variation_index)
            except Exception:
                selected_variation_index = -1
        
        return has_mapped_macros, variation_count, selected_variation_index
    
    def _send_rack_snapshot_state(self, all_devices=None):
        if all_devices is None:
            selected_track = self.song().view.selected_track
            if not selected_track or not hasattr(selected_track, 'devices'):
                self._send_sys_ex_message("", 0x30)
                return
            all_devices = self._get_all_nested_devices(selected_track.devices)[0]
        
        entries = []
        for index, device in enumerate(all_devices):
            info = self._rack_snapshot_info_for_device(device)
            if info is None:
                continue
            has_mapped_macros, variation_count, selected_variation_index = info
            entries.append("{}|{}|{}|{}".format(
                index + 1,
                1 if has_mapped_macros else 0,
                variation_count,
                selected_variation_index
            ))
        
        self._send_sys_ex_message(",".join(entries), 0x30)
    
    def _get_selected_drum_pad(self, drum_rack):
        """
        Get the currently selected drum pad from a drum rack device.
        Returns the selected drum pad or None if none is selected.
        """
        if hasattr(drum_rack, 'view') and hasattr(drum_rack.view, 'selected_drum_pad'):
            return drum_rack.view.selected_drum_pad
        
        # Alternative: if the view approach doesn't work, we could try to find the 
        # currently playing pad or default to the first pad with a chain
        for pad in drum_rack.drum_pads:
            if liveobj_valid(pad) and hasattr(pad, 'chains') and pad.chains:
                return pad
                
        return None
    
    def _send_selected_drum_pad_number(self):
        """
        Gets the currently selected drum pad number and sends it as a MIDI note on message
        on channel 3, note number 3, with the pad number encoded in the velocity.
        """
        try:
            if self._drum_rack_device:
                selected_pad = self._drum_rack_device.view.selected_drum_pad
                if selected_pad:
                    pad_number = selected_pad.note
                    now = time.time()
                    if self._last_drum_pad_note == pad_number and (now - self._last_drum_pad_change_at) < 0.05:
                        return
                    self._last_drum_pad_note = pad_number
                    self._last_drum_pad_change_at = now
                    
                    channel = 3
                    midi_note_number = 3
                    self.send_note_on(midi_note_number, channel, pad_number)
                    
                    if hasattr(self, '_metadata_recheck_timer') and self._metadata_recheck_timer:
                        self._metadata_recheck_timer.cancel()
                    
                    self._drum_pad_change_recheck_count = 0
                    self._last_drum_pad_metadata = None
                    self._last_sent_metadata = None
                    self._drum_pad_recheck_start = time.time()
                    
                    # Only auto-follow pad if we're already on a pad device
                    self._select_device_in_selected_drum_pad()
                    
                    # Kick off a metadata recheck loop to wait for full pad loading
                    self._debug_log("Drum pad change: starting metadata recheck loop")
                    self._metadata_recheck_timer = threading.Timer(0.1, self._recheck_parameter_metadata)
                    self._metadata_recheck_timer.start()
            else:
                self._debug_log("No drum pad selected")
            
        except Exception as e:
            self._debug_log(f"Error sending drum pad number: {str(e)}")
    
    def _find_mapped_device(self):
        """
        Finds the device that controls are currently mapped to by checking
        the device that owns the mapped parameters.
        """
        if not hasattr(self, '_device_controls'):
            return None
        
        for control in self._device_controls:
            if control and control.mapped_parameter():
                mapped_param = control.mapped_parameter()
                if hasattr(mapped_param, 'canonical_parent'):
                    device = mapped_param.canonical_parent
                    if hasattr(device, 'canonical_parent') and hasattr(device.canonical_parent, 'canonical_parent'):
                        return device
        return None

    def _select_device_in_selected_drum_pad(self):
        if not self._drum_rack_device:
            return
        
        selected_track = self.song().view.selected_track
        current_device = selected_track.view.selected_device
        if not current_device or not self._is_device_in_any_drum_pad(current_device):
            return
        
        selected_drum_pad = self._get_selected_drum_pad(self._drum_rack_device)
        if not selected_drum_pad or not selected_drum_pad.chains:
            return
        
        target_device = None
        mapped_device = self._find_mapped_device()
        if mapped_device and self._is_device_in_drum_pad(mapped_device):
            target_device = mapped_device
        else:
            def _first_device_in_chain(chain):
                if not liveobj_valid(chain) or not hasattr(chain, 'devices'):
                    return None
                for chain_device in chain.devices:
                    if liveobj_valid(chain_device):
                        if hasattr(chain_device, 'parameters') and chain_device.parameters:
                            return chain_device
                        if hasattr(chain_device, 'chains') and chain_device.chains:
                            for inner_chain in chain_device.chains:
                                nested = _first_device_in_chain(inner_chain)
                                if nested:
                                    return nested
                        return chain_device
                return None
            
            for chain in selected_drum_pad.chains:
                target_device = _first_device_in_chain(chain)
                if target_device:
                    break
        
        if target_device and liveobj_valid(target_device):
            try:
                if hasattr(self.song().view, 'select_device'):
                    self.song().view.select_device(target_device)
                elif hasattr(self._device, 'set_device'):
                    self._device.set_device(target_device)
                else:
                    selected_track.view.selected_device = target_device
            except Exception as e:
                self._debug_log(f"Error selecting drum pad device: {str(e)}")

    def _is_device_in_any_drum_pad(self, device):
        if not self._drum_rack_device or not device:
            return False
        
        def _device_in_chain(chain):
            if not liveobj_valid(chain) or not hasattr(chain, 'devices'):
                return False
            for chain_device in chain.devices:
                if chain_device == device:
                    return True
                if hasattr(chain_device, 'chains') and chain_device.chains:
                    for inner_chain in chain_device.chains:
                        if _device_in_chain(inner_chain):
                            return True
            return False
        
        for pad in self._drum_rack_device.drum_pads:
            if liveobj_valid(pad) and hasattr(pad, 'chains') and pad.chains:
                for chain in pad.chains:
                    if _device_in_chain(chain):
                        return True
        
        return False

    def _is_device_in_rack_chain(self, device, track_devices):
        if not device or not track_devices:
            return False
        
        parent_chain, _ = self._find_parent_chain(track_devices, device)
        return parent_chain is not None
    
    def _is_device_in_drum_pad(self, device):
        """
        Checks if a device is inside a drum pad of the current drum rack.
        """
        if not self._drum_rack_device or not device:
            return False
        
        selected_drum_pad = self._get_selected_drum_pad(self._drum_rack_device)
        if not selected_drum_pad or not selected_drum_pad.chains:
            return False
        
        def _device_in_chain(chain):
            if not liveobj_valid(chain) or not hasattr(chain, 'devices'):
                return False
            for chain_device in chain.devices:
                if chain_device == device:
                    return True
                if hasattr(chain_device, 'chains') and chain_device.chains:
                    for inner_chain in chain_device.chains:
                        if _device_in_chain(inner_chain):
                            return True
            return False
        
        for chain in selected_drum_pad.chains:
            if _device_in_chain(chain):
                return True
        
        return False

    def _send_parameter_info(self, parameter_names):
        if parameter_names == "":
            self._send_unmapped_parameter_metadata()
        else:
            selected_track = self.song().view.selected_track
            selected_device = selected_track.view.selected_device
            
            current_metadata = self._build_parameter_metadata(selected_device)
            
            if current_metadata:
                if self._metadata_is_all_unmapped(current_metadata) and hasattr(selected_device, 'parameters') and selected_device.parameters:
                    self._debug_log("Parameter metadata is all unmapped; sending placeholder and scheduling recheck")
                    self._send_unmapped_parameter_metadata_for_device(selected_device, current_metadata)
                    self._schedule_parameter_metadata_recheck(delay=0.05)
                    return False

                cached_metadata = self._get_cached_metadata(selected_device)
                metadata_changed = cached_metadata != current_metadata
                if metadata_changed:
                    self._send_sys_ex_message(current_metadata, 0x7D)
                    self._set_cached_metadata(selected_device, current_metadata)
                    self._mark_metadata_sent(selected_device)
                
                if metadata_changed:
                    unmapped_encoder_indices = []
                    for control_index in range(8):
                        mapped_param = self._current_connected_parameter_for_control(control_index, selected_device)
                        if not mapped_param:
                            unmapped_encoder_indices.append(control_index)
                    
                    for control_index in unmapped_encoder_indices:
                        cc_number = 72 + control_index
                        self.send_cc(cc_number, 8, 0)
                
                is_rack_device = isinstance(selected_device, Live.RackDevice.RackDevice)
                is_drum_rack = self._drum_rack_device and selected_device == self._drum_rack_device
                is_drum_pad_device = self._drum_rack_device and self._is_device_in_drum_pad(selected_device)
                is_rack_related = is_rack_device or self._is_device_in_rack_chain(selected_device, selected_track.devices)
                
                should_iterate = False
                if is_drum_rack or is_drum_pad_device:
                    should_iterate = self._metadata_has_unmapped(current_metadata)
                elif is_rack_related:
                    should_iterate = self._metadata_needs_rack_recheck(current_metadata)
                
                if should_iterate:
                    if is_drum_pad_device:
                        self._last_drum_pad_metadata = current_metadata
                    else:
                        self._last_sent_metadata = current_metadata
                    
                    if is_drum_rack or is_drum_pad_device:
                        self._drum_pad_change_recheck_count = 0
                        self._drum_pad_recheck_start = time.time()
                    else:
                        self._device_recheck_count = 0
                        self._device_recheck_start = time.time()
                    
                    self._schedule_parameter_metadata_recheck()
                else:
                    self._last_sent_metadata = None
                    self._last_drum_pad_metadata = None
                    self._drum_pad_change_recheck_count = 0

                self._refresh_parameter_value_listeners_current_bank(send_current_values=True)
                self._refresh_parameter_name_listeners_current_bank()
                self._refresh_automation_state_listeners_current_bank()
                return True
        return False

    def _send_sys_ex_message(self, name_string, manufacturer_id):
        status_byte = 0xF0  # SysEx message start
        end_byte = 0xF7  # SysEx message end
        device_id = 0x01
        name_string = self._sanitize_sysex_text(name_string)
        data = name_string.encode('ascii', errors='ignore')
        max_chunk_length = 240
        if len(data) <= max_chunk_length:
            sys_ex_message = (status_byte, manufacturer_id, device_id) + tuple(data) + (end_byte, )
            self._send_midi(sys_ex_message)
        else:
            num_of_chunks = (len(data) + max_chunk_length - 1) // max_chunk_length
            for chunk_index in range(num_of_chunks):
                start_index = chunk_index * max_chunk_length
                end_index = start_index + max_chunk_length
                prefix = "$"
                if chunk_index == num_of_chunks - 1:
                    prefix = "_"
                chunk_data = prefix.encode('ascii') + data[start_index:end_index]

                sys_ex_message = (status_byte, manufacturer_id, device_id) + tuple(chunk_data) + (end_byte, )
                self._send_midi(sys_ex_message)

    def _initialize_buttons(self):
        transport.set_metronome_button(ButtonElement(1, MIDI_CC_TYPE, 0, 58))
        session_component.set_stop_all_clips_button(ButtonElement(1, MIDI_NOTE_TYPE, 15, 96))
        self.capture_button = ButtonElement(True, MIDI_NOTE_TYPE, 15, 100)
        self.capture_button.add_value_listener(self._capture_button_value)
        self.quantize_button = ButtonElement(True, MIDI_NOTE_TYPE, 15, 99)
        self.quantize_button.add_value_listener(self._quantize_button_value)
        # duplicate the active clip to a free slot
        self.duplicate_button = ButtonElement(True, MIDI_NOTE_TYPE, 15, 98)
        self.duplicate_button.add_value_listener(self._duplicate_button_value)
        # duplicate scene
        self.duplicate_scene_button = ButtonElement(True, MIDI_NOTE_TYPE, 15, 95)
        self.duplicate_scene_button.add_value_listener(self._duplicate_scene_button_value)
        # a session recording button
        self.sesh_record_button = ButtonElement(1, MIDI_CC_TYPE, 0, 119)
        self.sesh_record_button.add_value_listener(self._sesh_record_value)
        # quantize grid size button
        quantize_grid_button = ButtonElement(1, MIDI_CC_TYPE, 1, 0)
        quantize_grid_button.add_value_listener(self._quantize_grid_value)
        # quantize strength
        quantize_strength_button = ButtonElement(1, MIDI_CC_TYPE, 1, 1)
        quantize_strength_button.add_value_listener(self._quantize_strength_value)
        # swing percentage button
        swing_amount_button = ButtonElement(1, MIDI_CC_TYPE, 1, 2)
        swing_amount_button.add_value_listener(self._swing_amount_value)
        # # periodic check
        # periodic_check_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 97)
        # periodic_check_button.add_value_listener(self._periodic_check)
        # redo button
        self.redo_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 102)
        self.redo_button.add_value_listener(self._redo_button_value)
        # undo button
        self.undo_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 101)
        self.undo_button.add_value_listener(self._undo_button_value)
        # device selection
        device_selection_button = ButtonElement(1, MIDI_CC_TYPE, 1, 3)
        device_selection_button.add_value_listener(self._select_device_by_index)
        # track selection
        track_selection_button = ButtonElement(1, MIDI_CC_TYPE, 1, 4)
        track_selection_button.add_value_listener(self._select_track_by_index)
        # return and master track selection
        return_track_selection_button = ButtonElement(1, MIDI_CC_TYPE, 1, 5)
        return_track_selection_button.add_value_listener(self._select_return_track_by_index)
        # scene launch
        scene_launch_button = ButtonElement(1, MIDI_CC_TYPE, 1, 14)
        scene_launch_button.add_value_listener(self._fire_scene)
        # clip / scene select
        clip_scene_select_button = ButtonElement(1, MIDI_CC_TYPE, 1, 15)
        clip_scene_select_button.add_value_listener(self._select_clip_scene)
        # scene delete
        scene_delete_button = ButtonElement(1, MIDI_CC_TYPE, 1, 16)
        scene_delete_button.add_value_listener(self._delete_scene)
        # random device add button
        random_device_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 93)
        random_device_button.add_value_listener(self._add_random_sound)
        # random audio effect button
        random_effect_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 92)
        random_effect_button.add_value_listener(self._add_random_effect)
        # random synth button
        random_synth_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 103)
        random_synth_button.add_value_listener(self._add_random_synth)
        # random drums button
        random_drums_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 104)
        random_drums_button.add_value_listener(self._add_random_drums)
        # delete device button
        delete_device_button = ButtonElement(1, MIDI_CC_TYPE, 1, 17)
        delete_device_button.add_value_listener(self._delete_device)
        # select drum pad button
        select_drum_pad_button = ButtonElement(1, MIDI_CC_TYPE, 1, 24)
        select_drum_pad_button.add_value_listener(self._select_drum_pad)
        # move device left
        move_device_left_button = ButtonElement(1, MIDI_CC_TYPE, 1, 18)
        move_device_left_button.add_value_listener(self._move_device_left)
        # move device right
        move_device_right_button = ButtonElement(1, MIDI_CC_TYPE, 1, 19)
        move_device_right_button.add_value_listener(self._move_device_right)
        # add midi track
        add_midi_track_button = ButtonElement(1, MIDI_CC_TYPE, 1, 21)
        add_midi_track_button.add_value_listener(self._add_midi_track)
        # delete midi track
        delete_midi_track_button = ButtonElement(1, MIDI_CC_TYPE, 1, 22)
        delete_midi_track_button.add_value_listener(self._delete_midi_track)
        # add return track
        add_return_track_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 91)
        add_return_track_button.add_value_listener(self._add_return_track)
        # delete return track
        delete_return_track_button = ButtonElement(1, MIDI_CC_TYPE, 1, 23)
        delete_return_track_button.add_value_listener(self._delete_return_track)
        # mixer view status
        mixer_view_status = ButtonElement(1, MIDI_NOTE_TYPE, 15, 90)
        mixer_view_status.add_value_listener(self._update_mixer_status)
        # device view status
        device_view_status = ButtonElement(1, MIDI_NOTE_TYPE, 15, 89)
        device_view_status.add_value_listener(self._update_device_status)
        # step seq status
        step_seq_status = ButtonElement(1, MIDI_NOTE_TYPE, 15, 87)
        step_seq_status.add_value_listener(self._update_step_seq)
        # adding empty clip
        add_empty_clip_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 86)
        add_empty_clip_button.add_value_listener(self._add_empty_clip)
        # creating new empty clip
        create_empty_clip_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 85)
        create_empty_clip_button.add_value_listener(self._create_new_empty_clip)
        # selecting playing clip
        select_playing_clip_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 84)
        select_playing_clip_button.add_value_listener(self._select_playing_clip)
        # cropping selected clip
        crop_clip_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 83)
        crop_clip_button.add_value_listener(self._crop_clip)
        # browser start button (CC channel 1, CC 25)
        self.browser_start_button = ButtonElement(1, MIDI_CC_TYPE, 1, 25)
        self.browser_start_button.add_value_listener(self._start_browser)
        # browser pagination button (MIDI note channel 15, note 82)
        self.browser_navigate_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 82)
        self.browser_navigate_button.add_value_listener(self._browser_navigate)
        # browser select item button (MIDI note channel 15, note 81)
        self.browser_open_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 81)
        self.browser_open_button.add_value_listener(self._browser_open_item)
        # browser load item button (MIDI note channel 15, note 80) - always loads, never opens children
        self.browser_load_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 80)
        self.browser_load_button.add_value_listener(self._browser_load_item)
        # browser go back button (MIDI note channel 15, note 79) - go one level back, remembers page
        self.browser_back_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 79)
        self.browser_back_button.add_value_listener(self._browser_go_back)
        self.re_enable_automation_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 78)
        self.re_enable_automation_button.add_value_listener(self._re_enable_automation)
        self.remove_automation_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 77)
        self.remove_automation_button.add_value_listener(self._arm_remove_automation_from_next_encoder)
        self.re_enable_parameter_automation_button = ButtonElement(1, MIDI_NOTE_TYPE, 15, 76)
        self.re_enable_parameter_automation_button.add_value_listener(self._arm_re_enable_automation_from_next_encoder)
        # direct bank selection (CC channel 11, CC 64) - value = 64 + offset
        bank_select_button = ButtonElement(1, MIDI_CC_TYPE, 11, 64)
        bank_select_button.add_value_listener(self._bank_select)


    def send_note_on(self, note_number, channel, velocity):
        channel_byte = channel & 0x7F
        note_byte = note_number & 0x7F
        velocity_byte = velocity & 0x7F
        midi_note_on_message = (0x90 | channel_byte, note_byte, velocity_byte)
        self._send_midi(midi_note_on_message)

    def send_note_off(self, note_number, channel, velocity):
        channel_byte = channel & 0x7F
        note_byte = note_number & 0x7F
        velocity_byte = velocity & 0x7F
        midi_note_off_message = (0x80 | channel_byte, note_byte, velocity_byte)
        self._send_midi(midi_note_off_message)

    def send_cc(self, cc_number, channel, value):
        channel_byte = channel & 0x7F
        cc_byte = cc_number & 0x7F
        value_byte = int(round(value)) & 0x7F
        midi_cc_message = (0xB0 | channel_byte, cc_byte, value_byte)
        self._send_midi(midi_cc_message)

    def _parameter_value_to_cc(self, device_param):
        if not hasattr(device_param, 'value') or not hasattr(device_param, 'min') or not hasattr(device_param, 'max'):
            return 0
        
        value = device_param.value
        min_val = device_param.min
        max_val = device_param.max
        
        if max_val == min_val:
            return 0
        
        normalized = (value - min_val) / (max_val - min_val)
        cc_value = int(round(normalized * 127))
        return max(0, min(127, cc_value))

    def _create_disabled_param_listener(self, device_param, control_index):
        def listener():
            if not self._is_current_parameter_for_control(control_index, device_param):
                return
            self._send_parameter_feedback(control_index, device_param, force_display=True)
        return listener

    def _create_parameter_value_listener(self, device_param, control_index):
        def listener():
            if not self._is_current_parameter_for_control(control_index, device_param):
                return
            self._send_parameter_feedback(control_index, device_param)
        return listener

    def _create_parameter_name_listener(self, device_param, control_index):
        def listener():
            if not self._is_current_parameter_for_control(control_index, device_param):
                return
            self._schedule_active_bank_parameter_refresh()
        return listener

    def _create_parameter_source_listener(self):
        def listener():
            self._schedule_active_bank_parameter_refresh()
        return listener

    def _create_bank_parameter_source_listener(self):
        def listener():
            self._schedule_active_bank_parameter_refresh()
        return listener

    def _schedule_active_bank_parameter_refresh(self):
        if not liveobj_valid(self._device):
            return

        if self._parameter_name_update_timer:
            self._parameter_name_update_timer.cancel()

        self._parameter_name_update_timer = threading.Timer(0.05, self._refresh_active_bank_parameters)
        self._parameter_name_update_timer.start()

    def _refresh_active_bank_parameters(self):
        self._parameter_name_update_timer = None
        if not liveobj_valid(self._device):
            return

        try:
            if hasattr(self._device, 'update'):
                self._device.update()
            self._on_device_changed()
        except Exception as e:
            self._debug_log("Error refreshing active bank after parameter metadata change: {}".format(str(e)))

    def _remove_parameter_value_listeners(self):
        for (param, control_index), listener in list(getattr(self, '_parameter_value_listeners', {}).items()):
            if liveobj_valid(param) and hasattr(param, 'remove_value_listener'):
                try:
                    if param.value_has_listener(listener):
                        param.remove_value_listener(listener)
                except Exception:
                    pass
        self._parameter_value_listeners.clear()
        self._last_sent_parameter_displays.clear()
        self._last_sent_parameter_normalized_values.clear()
        self._last_sent_parameter_cc_values.clear()
        self._last_parameter_display_feedback_times.clear()

    def _remove_parameter_name_listeners(self):
        if self._parameter_name_update_timer:
            self._parameter_name_update_timer.cancel()
            self._parameter_name_update_timer = None

        for (param, control_index), listener in list(getattr(self, '_parameter_name_listeners', {}).items()):
            if liveobj_valid(param) and hasattr(param, 'remove_name_listener'):
                try:
                    if not hasattr(param, 'name_has_listener') or param.name_has_listener(listener):
                        param.remove_name_listener(listener)
                except Exception:
                    pass
        self._parameter_name_listeners.clear()

    def _remove_parameter_source_listener(self):
        device = getattr(self, '_parameter_source_device', None)
        listener = getattr(self, '_parameter_source_listener', None)
        if device and listener and liveobj_valid(device) and hasattr(device, 'remove_parameters_listener'):
            try:
                if not hasattr(device, 'parameters_has_listener') or device.parameters_has_listener(listener):
                    device.remove_parameters_listener(listener)
            except Exception:
                pass
        self._parameter_source_device = None
        self._parameter_source_listener = None

        bank_device = getattr(self, '_bank_parameter_source_device', None)
        bank_listener = getattr(self, '_bank_parameter_source_listener', None)
        if bank_device and bank_listener and liveobj_valid(bank_device) and hasattr(bank_device, 'remove_bank_parameters_changed_listener'):
            try:
                if not hasattr(bank_device, 'bank_parameters_changed_has_listener') or bank_device.bank_parameters_changed_has_listener(bank_listener):
                    bank_device.remove_bank_parameters_changed_listener(bank_listener)
            except Exception:
                pass
        self._bank_parameter_source_device = None
        self._bank_parameter_source_listener = None

    def _set_parameter_source_listener(self, selected_device):
        if selected_device == getattr(self, '_parameter_source_device', None):
            return

        self._remove_parameter_source_listener()
        if not selected_device or not liveobj_valid(selected_device):
            return

        if hasattr(selected_device, 'add_parameters_listener'):
            listener = self._create_parameter_source_listener()
            self._parameter_source_device = selected_device
            self._parameter_source_listener = listener
            try:
                if not hasattr(selected_device, 'parameters_has_listener') or not selected_device.parameters_has_listener(listener):
                    selected_device.add_parameters_listener(listener)
            except Exception:
                self._parameter_source_device = None
                self._parameter_source_listener = None

        if hasattr(selected_device, 'add_bank_parameters_changed_listener'):
            bank_listener = self._create_bank_parameter_source_listener()
            self._bank_parameter_source_device = selected_device
            self._bank_parameter_source_listener = bank_listener
            try:
                if not hasattr(selected_device, 'bank_parameters_changed_has_listener') or not selected_device.bank_parameters_changed_has_listener(bank_listener):
                    selected_device.add_bank_parameters_changed_listener(bank_listener)
            except Exception:
                self._bank_parameter_source_device = None
                self._bank_parameter_source_listener = None

    def _refresh_parameter_value_listeners_current_bank(self, send_current_values=False):
        self._remove_parameter_value_listeners()
        if not hasattr(self, '_device') or not liveobj_valid(self._device):
            return
        if not hasattr(self._device, '_parameter_controls'):
            return

        selected_device = self._selected_device()
        for control_index, control in enumerate(self._device._parameter_controls):
            mapped_param = self._current_connected_parameter_for_control(control_index, selected_device)
            if mapped_param and liveobj_valid(mapped_param) and hasattr(mapped_param, 'add_value_listener'):
                listener = self._create_parameter_value_listener(mapped_param, control_index)
                self._parameter_value_listeners[(mapped_param, control_index)] = listener
                try:
                    if not mapped_param.value_has_listener(listener):
                        mapped_param.add_value_listener(listener)
                except Exception:
                    pass
                self._last_sent_parameter_cc_values[control_index] = self._parameter_value_to_cc(mapped_param)
                self._last_sent_parameter_displays[control_index] = self._parameter_display_value(mapped_param)
                self._last_sent_parameter_normalized_values[control_index] = round(self._parameter_normalized_value(mapped_param), 9)
                if send_current_values:
                    self._send_parameter_feedback(control_index, mapped_param, force_display=True)

    def _refresh_parameter_name_listeners_current_bank(self):
        self._remove_parameter_name_listeners()
        if not hasattr(self, '_device') or not liveobj_valid(self._device):
            return
        if not hasattr(self._device, '_parameter_controls'):
            return

        selected_device = self._selected_device()
        for control_index, control in enumerate(self._device._parameter_controls):
            mapped_param = self._current_connected_parameter_for_control(control_index, selected_device)
            if mapped_param and liveobj_valid(mapped_param) and hasattr(mapped_param, 'add_name_listener'):
                listener = self._create_parameter_name_listener(mapped_param, control_index)
                self._parameter_name_listeners[(mapped_param, control_index)] = listener
                try:
                    if not hasattr(mapped_param, 'name_has_listener') or not mapped_param.name_has_listener(listener):
                        mapped_param.add_name_listener(listener)
                except Exception:
                    pass

    def _create_automation_state_listener(self, device_param):
        def listener():
            self._refresh_parameter_metadata_on_automation_change()
        return listener

    def _refresh_automation_state_listeners_current_bank(self):
        self._remove_automation_state_listeners()
        if not hasattr(self, '_device') or not liveobj_valid(self._device):
            return
        selected_device = self._selected_device()
        for control_index, control in enumerate(self._device._parameter_controls):
            device_param = self._current_connected_parameter_for_control(control_index, selected_device)
            if device_param and liveobj_valid(device_param):
                listener = self._create_automation_state_listener(device_param)
                if device_param not in self._automation_state_listeners:
                    try:
                        add_listener = getattr(device_param, 'add_automation_state_listener', None)
                        has_listener = getattr(device_param, 'automation_state_has_listener', None)
                        if add_listener and (not has_listener or not has_listener(listener)):
                            add_listener(listener)
                            self._automation_state_listeners[device_param] = listener
                    except Exception:
                        pass

    def _get_automation_signature(self):
        if not hasattr(self, '_device') or not liveobj_valid(self._device):
            return None
        signature = []
        selected_device = self._selected_device()
        for control_index, control in enumerate(self._device._parameter_controls):
            mapped_param = self._current_connected_parameter_for_control(control_index, selected_device)
            if mapped_param and hasattr(mapped_param, 'automation_state'):
                signature.append(mapped_param.automation_state)
            else:
                signature.append(None)
        return tuple(signature)

    def _refresh_parameter_metadata_on_automation_change(self):
        if not liveobj_valid(self._device):
            return
        
        selected_track = self.song().view.selected_track
        if self._track_device_is_selected():
            self._send_track_device_parameter_metadata(selected_track)
            self._last_automation_signature = self._get_automation_signature()
            self._send_re_enable_automation_enabled(force=True)
            return

        selected_device = selected_track.view.selected_device
        
        if not selected_device or not hasattr(selected_device, 'parameters'):
            return

        current_signature = self._get_automation_signature()
        if current_signature is not None and current_signature == self._last_automation_signature:
            return
        
        # Update signature immediately so the same change doesn't re-trigger
        # while the timer is pending
        self._last_automation_signature = current_signature
        
        with self._automation_timer_lock:
            if self._automation_metadata_update_timer:
                self._automation_metadata_update_timer.cancel()
        
        self._automation_metadata_device_id = id(selected_device)
        self._automation_metadata_retry_count = 0
        self._automation_metadata_retry_start = time.time()
        seq_at_schedule = self._metadata_send_seq_by_device.get(id(selected_device), 0)
        new_timer = threading.Timer(
            0.05,
            self._send_refreshed_parameter_metadata,
            args=[selected_device, seq_at_schedule]
        )
        with self._automation_timer_lock:
            self._automation_metadata_update_timer = new_timer
        new_timer.start()

    def _send_refreshed_parameter_metadata(self, selected_device, seq_at_schedule=0):
        with self._automation_timer_lock:
            self._automation_metadata_update_timer = None
        if not selected_device or not liveobj_valid(selected_device):
            return
        if self._metadata_send_seq_by_device.get(id(selected_device), 0) != seq_at_schedule:
            return
        current_selected = self.song().view.selected_track.view.selected_device
        if current_selected != selected_device or id(selected_device) != self._automation_metadata_device_id:
            return
        current_metadata = self._build_parameter_metadata(selected_device)
        if current_metadata:
            if self._metadata_is_all_unmapped(current_metadata) and hasattr(selected_device, 'parameters') and selected_device.parameters:
                elapsed = 0.0
                if self._automation_metadata_retry_start is not None:
                    elapsed = time.time() - self._automation_metadata_retry_start
                cached_metadata = self._get_cached_metadata(selected_device)
                if cached_metadata != current_metadata:
                    self._send_unmapped_parameter_metadata_for_device(selected_device, current_metadata)
                    seq_at_schedule = self._metadata_send_seq_by_device.get(id(selected_device), seq_at_schedule)
                if self._automation_metadata_retry_count < 3 and elapsed < 0.3:
                    self._automation_metadata_retry_count += 1
                    new_timer = threading.Timer(
                        0.05,
                        self._send_refreshed_parameter_metadata,
                        args=[selected_device, seq_at_schedule]
                    )
                    with self._automation_timer_lock:
                        self._automation_metadata_update_timer = new_timer
                    new_timer.start()
                return

            cached_metadata = self._get_cached_metadata(selected_device)
            metadata_changed = cached_metadata != current_metadata
            if metadata_changed:
                self._send_sys_ex_message(current_metadata, 0x7D)
                self._schedule_parameter_metadata_resend(selected_device, current_metadata, seq_at_schedule)
                self._set_cached_metadata(selected_device, current_metadata)
                self._mark_metadata_sent(selected_device)
                self._last_automation_signature = self._get_automation_signature()
                self._refresh_parameter_value_listeners_current_bank(send_current_values=True)
                self._refresh_parameter_name_listeners_current_bank()
                self._refresh_automation_state_listeners_current_bank()
            
            if metadata_changed and hasattr(selected_device, 'parameters') and selected_device.parameters:
                for control_index in range(8):
                    mapped_param = self._current_connected_parameter_for_control(control_index, selected_device)
                    if mapped_param:
                        self._send_parameter_feedback(control_index, mapped_param, force_display=True)
            
            if not metadata_changed:
                elapsed = 0.0
                if self._automation_metadata_retry_start is not None:
                    elapsed = time.time() - self._automation_metadata_retry_start
                if self._automation_metadata_retry_count < 3 and elapsed < 0.3:
                    self._automation_metadata_retry_count += 1
                    new_timer = threading.Timer(
                        0.05,
                        self._send_refreshed_parameter_metadata,
                        args=[selected_device, seq_at_schedule]
                    )
                    with self._automation_timer_lock:
                        self._automation_metadata_update_timer = new_timer
                    new_timer.start()
                else:
                    self._automation_metadata_retry_count = 0
                    self._automation_metadata_retry_start = None
                    self._last_automation_signature = self._get_automation_signature()
            else:
                self._automation_metadata_retry_count = 0
                self._automation_metadata_retry_start = None

    def _schedule_parameter_metadata_resend(self, selected_device, metadata, seq_at_schedule):
        timer = threading.Timer(
            0.1,
            self._resend_parameter_metadata_if_current,
            args=[selected_device, metadata, seq_at_schedule]
        )
        timer.start()

    def _resend_parameter_metadata_if_current(self, selected_device, metadata, seq_at_schedule):
        try:
            if not selected_device or not liveobj_valid(selected_device):
                return
            if self._metadata_send_seq_by_device.get(id(selected_device), 0) != seq_at_schedule:
                return
            current_selected = self.song().view.selected_track.view.selected_device
            if current_selected == selected_device:
                self._send_sys_ex_message(metadata, 0x7D)
        except Exception:
            pass

    def _remove_disabled_parameter_listeners(self):
        for (param, control_index), listener in self._disabled_parameter_listeners.items():
            if liveobj_valid(param) and hasattr(param, 'remove_value_listener'):
                if param.value_has_listener(listener):
                    param.remove_value_listener(listener)
        self._disabled_parameter_listeners.clear()
        self._disabled_parameters.clear()
        self._current_disabled_controls.clear()

    def _remove_automation_state_listeners(self):
        for param, listener in self._automation_state_listeners.items():
            if liveobj_valid(param) and hasattr(param, 'remove_automation_state_listener'):
                if hasattr(param, 'automation_state_has_listener') and param.automation_state_has_listener(listener):
                    param.remove_automation_state_listener(listener)
        self._automation_state_listeners.clear()
        self._automation_metadata_device_id = None
        self._automation_metadata_retry_count = 0
        self._automation_metadata_retry_start = None
        self._last_automation_signature = None
        
        if self._automation_metadata_update_timer:
            self._automation_metadata_update_timer.cancel()
            self._automation_metadata_update_timer = None

    def _readd_disabled_parameter_listeners(self):
        if not hasattr(self, '_device') or not liveobj_valid(self._device):
            return
        
        selected_device = self._selected_device()
        
        if hasattr(selected_device, 'parameters') and selected_device.parameters:
            for control_index, control in enumerate(self._device._parameter_controls):
                device_param = self._current_connected_parameter_for_control(control_index, selected_device)

                if device_param and hasattr(device_param, 'is_enabled') and not device_param.is_enabled:
                    if (device_param, control_index) not in self._disabled_parameter_listeners:
                        listener = self._create_disabled_param_listener(device_param, control_index)
                        if not device_param.value_has_listener(listener):
                            device_param.add_value_listener(listener)

                        self._disabled_parameter_listeners[(device_param, control_index)] = listener
                        self._disabled_parameters.append(device_param)
                        self._current_disabled_controls.append(control_index)

                        self._send_parameter_feedback(control_index, device_param, force_display=True)

    def _connection_established(self, value):
        if value:            
            # self.log_message("Connection App to Ableton (still) works!")
            # send midi note on channel 3, note number 1 to confirm handshake
            midi_event_bytes = (0x90 | 0x03, 0x01, secret_version_number)
            self._send_midi(midi_event_bytes)
            was_initialized = self.was_initialized
            
            # initializing everything else if this is not just the handshake
            if self.was_initialized is False:
                self.was_initialized = True
                self.old_clips_array = []
                self._follow_action_track_signature = self._track_signature(self.song().tracks)
                self._on_tracks_changed()
                song = self.song()
                self._initialize_buttons()
                self._update_mixer_and_tracks()
                self._set_selected_track_implicit_arm()
                selected_track = song.view.selected_track
                self._send_selected_track_index(selected_track)
                self._on_selected_track_changed.subject = song.view
                # updating scale
                self._on_scale_changed()
                # track = self.song().view.selected_track
                # track.view.add_selected_device_listener(self._on_selected_device_changed)
                # self.song().view.add_selected_scene_listener(self._on_selected_scene_changed)
                self._ensure_song_listeners(song)
                # updating tempo and metronome
                self._update_tempo()
                self._update_metronome()
                # rest
                self._setup_device_control()
                self._register_clip_listeners()
                self._send_current_project_state()
                self._start_periodic_execution()
            
            # hack to get new tracks if we have a new song.
            did_send_new_song = self._check_for_new_song()
            if was_initialized and not did_send_new_song:
                self._send_current_project_state()

    def _transport_toggle_value(self, value):
        if value:
            try:
                if self._song_is_playing():
                    self.song().stop_playing()
                else:
                    self.song().start_playing()
                self._send_transport_state(force=True)
            except Exception:
                pass

    def _send_project(self, value):
        if value:
            self._send_current_project_state()

    def _start_periodic_execution(self):
        self.periodic_timer = 1
        if self._periodic_timer_ref is None or not self._periodic_timer_ref.is_alive():
            self._periodic_execution()
    
    def _periodic_execution(self):
        self._periodic_check()
        if self.periodic_timer == 1:
            self._periodic_timer_ref = threading.Timer(0.3, self._periodic_execution)
            self._periodic_timer_ref.start()

    def _periodic_check(self):
        if self.was_initialized:
            self._check_for_new_song()
        else:
            self._check_for_follow_action_song_change()
        self._sync_follow_actions_to_track_topology()
        if self._has_follow_action_runtime_work():
            self._reconcile_follow_action_rules()
            self._sync_follow_action_runtime_listeners()
        self._sync_follow_actions_to_transport()
        self._evaluate_follow_actions()
        self._evaluate_mutator_regeneration()
        if not self.was_initialized:
            return
        self._send_group_fold_states_if_changed()
        # update clip slots
        # we only need to update clip slots periodically when we are in clip slots view
        # meaning not in the device view
        if self.device_status is False:
            self._update_clip_slots()

    def _has_follow_action_runtime_work(self):
        return bool(
            self._follow_action_rules
            or self._active_follow_actions
            or self._handled_follow_action_launches
            or self._follow_action_missing_clip_counts
            or self._follow_action_scene_triggered_listeners
            or self._follow_action_clip_slot_runtime_listeners
        )

    def _ensure_song_listener(self, song, listener_name, listener):
        try:
            has_listener = getattr(song, "{}_has_listener".format(listener_name), None)
            add_listener = getattr(song, "add_{}_listener".format(listener_name), None)
            if add_listener and (not has_listener or not has_listener(listener)):
                add_listener(listener)
        except Exception:
            pass

    def _ensure_song_listeners(self, song):
        self._ensure_song_listener(song, "tracks", self._on_tracks_changed)
        self._ensure_song_listener(song, "scale_name", self._on_scale_changed)
        self._ensure_song_listener(song, "root_note", self._on_scale_changed)
        self._ensure_song_listener(song, "tempo", self._update_tempo)
        self._ensure_song_listener(song, "metronome", self._update_metronome)
        self._ensure_song_listener(song, "session_record", self._on_session_record_changed)
        try:
            if (not hasattr(song, "is_playing_has_listener")
                    or not song.is_playing_has_listener(self._on_song_is_playing_changed)):
                song.add_is_playing_listener(self._on_song_is_playing_changed)
        except Exception:
            pass
        self._ensure_song_listener(song, "re_enable_automation_enabled", self._on_re_enable_automation_enabled_changed)

    def _remove_song_listener(self, song, listener_name, listener):
        try:
            has_listener = getattr(song, "{}_has_listener".format(listener_name), None)
            remove_listener = getattr(song, "remove_{}_listener".format(listener_name), None)
            if remove_listener and (not has_listener or has_listener(listener)):
                remove_listener(listener)
        except Exception:
            pass

    def _ensure_follow_action_song_listeners(self, song):
        if self._follow_action_song_listener_subject is not None and self._follow_action_song_listener_subject != song:
            self._remove_follow_action_song_listeners()
        self._follow_action_song_listener_subject = song
        self._ensure_song_listener(song, "tracks", self._on_follow_action_topology_changed)
        self._ensure_song_listener(song, "scenes", self._on_follow_action_topology_changed)

    def _remove_follow_action_song_listeners(self):
        song = self._follow_action_song_listener_subject
        if song is None:
            return
        self._remove_song_listener(song, "tracks", self._on_follow_action_topology_changed)
        self._remove_song_listener(song, "scenes", self._on_follow_action_topology_changed)
        self._follow_action_song_listener_subject = None

    def _on_follow_action_topology_changed(self):
        self._sync_follow_actions_to_track_topology()
        self._sync_follow_action_name_listeners()
        self._load_follow_actions_from_names()
        self._sync_follow_action_runtime_listeners()

    def _on_follow_action_name_changed(self):
        self._load_follow_actions_from_names()
        self._sync_follow_action_runtime_listeners()
        self._evaluate_follow_actions()

    def _on_follow_action_timing_changed(self):
        self._evaluate_follow_actions()

    def _send_selected_track_state(self):
        try:
            selected_track = self.song().view.selected_track
            self._send_selected_track_index(selected_track)
            track_has_midi_input = 1 if selected_track and selected_track.has_midi_input else 0
            self._send_sys_ex_message(str(track_has_midi_input), 0x0B)
        except Exception:
            pass

    def _send_selected_device_state(self):
        try:
            selected_track = self.song().view.selected_track
            selected_device = selected_track.view.selected_device if selected_track else None
            if self._track_device_is_selected():
                self._last_automation_signature = None
                self._on_device_changed()
                return
            if hasattr(self, "_device") and hasattr(self._device, "set_device"):
                self._device.set_device(selected_device)
            if selected_device:
                self._clear_cached_metadata(selected_device)
            self._last_automation_signature = None
            self._on_device_changed()
        except Exception:
            pass

    def _send_current_project_state(self):
        self.old_clips_array = []
        self._send_transport_state(force=True)
        self._send_session_record_state(force=True)
        self._update_tempo()
        self._update_metronome()
        self._send_re_enable_automation_enabled()
        self._update_mixer_and_tracks()
        self._send_selected_track_state()
        self._send_selected_device_state()
        self._load_follow_actions_from_names(force_send=True)
        self._update_clip_slots()
        self._check_clip_playing_status(force=True)

    def _re_enable_automation_value(self):
        try:
            song = self.song()
            if hasattr(song, "re_enable_automation_enabled"):
                return bool(song.re_enable_automation_enabled)
        except Exception:
            pass
        return False

    def _send_re_enable_automation_enabled(self, force=False):
        enabled = self._re_enable_automation_value()
        if not force and enabled == self._re_enable_automation_enabled_state:
            return
        self._re_enable_automation_enabled_state = enabled
        self._send_sys_ex_message("1" if enabled else "0", 0x29)
        if getattr(self, 'mixer_status', False):
            self._schedule_mixer_automation_status_resends()

    def _on_re_enable_automation_enabled_changed(self):
        self._send_re_enable_automation_enabled()

    def _check_for_new_song(self):
        if not self.was_initialized:
            return False
        current_song = self.song()
        if current_song == self.song_instance:
            return False

        self.song_instance = current_song
        self._track_list_signature = None
        self._last_group_fold_states = None
        self._last_group_hidden_states = None
        self._follow_action_track_signature = self._track_signature(current_song.tracks)
        self._follow_action_rules = {}
        self._active_follow_actions = {}
        self._handled_follow_action_launches = set()
        self._follow_action_missing_clip_counts = {}
        self._last_follow_action_state = None
        self._remove_follow_action_runtime_listeners()
        self._remove_follow_action_name_listeners()
        self._ensure_follow_action_song_listeners(current_song)
        self._ensure_song_listeners(current_song)
        self._on_selected_track_changed.subject = current_song.view
        self._update_tempo()
        self._on_scale_changed()
        self._register_clip_listeners()
        self._send_current_project_state()
        return True

    def _check_for_follow_action_song_change(self):
        current_song = self.song()
        if current_song == self.song_instance:
            return False

        self.song_instance = current_song
        self._follow_action_track_signature = self._track_signature(current_song.tracks)
        self._follow_action_rules = {}
        self._active_follow_actions = {}
        self._handled_follow_action_launches = set()
        self._follow_action_missing_clip_counts = {}
        self._last_follow_action_state = None
        self._remove_follow_action_runtime_listeners()
        self._remove_follow_action_name_listeners()
        self._ensure_follow_action_song_listeners(current_song)
        self._sync_follow_action_name_listeners()
        self._load_follow_actions_from_names(force_send=True)
        self._sync_follow_action_runtime_listeners()
        return True

    def _follow_action_key(self, target_kind, track_index, scene_index):
        if target_kind == "clip":
            return ("clip", int(track_index), int(scene_index))
        return ("scene", int(scene_index))

    def _live_object_identity(self, live_object):
        try:
            live_ptr = live_object._live_ptr
            if callable(live_ptr):
                return int(live_ptr())
            return int(live_ptr)
        except Exception:
            return id(live_object)

    def _track_signature(self, tracks):
        return tuple(self._live_object_identity(track) for track in tracks)

    def _find_clip_follow_action_rule(self, track_index, scene_index):
        try:
            clip_slot = self.song().tracks[track_index].clip_slots[scene_index]
            if not clip_slot.has_clip:
                return None, None
            key = self._follow_action_key("clip", track_index, scene_index)
            return key, self._follow_action_rules.get(key)
        except Exception:
            return None, None

    def _copy_clip_follow_action_rule(self, from_track, from_clip, to_track, to_clip, remove_source=False):
        source_key, source_rule = self._find_clip_follow_action_rule(from_track, from_clip)
        if not source_rule:
            return

        try:
            dest_slot = self.song().tracks[to_track].clip_slots[to_clip]
            if not dest_slot.has_clip:
                return
        except Exception:
            return

        if remove_source and source_key in self._follow_action_rules:
            del self._follow_action_rules[source_key]
            self._remove_follow_action_rule_from_name("clip", from_track, from_clip)
            self._handled_follow_action_launches.discard(source_key)
            self._follow_action_missing_clip_counts.pop(source_key, None)

        new_rule = dict(source_rule)
        new_rule["track_index"] = to_track
        new_rule["scene_index"] = to_clip

        dest_key = self._follow_action_key("clip", to_track, to_clip)
        self._follow_action_rules[dest_key] = new_rule
        self._save_follow_action_rule_to_name(new_rule)

    def _remove_clip_follow_action_rule(self, track_index, scene_index):
        key, _ = self._find_clip_follow_action_rule(track_index, scene_index)
        if key and key in self._follow_action_rules:
            del self._follow_action_rules[key]
        self._remove_follow_action_rule_from_name("clip", track_index, scene_index)
        if key in self._active_follow_actions:
            del self._active_follow_actions[key]
        self._handled_follow_action_launches.discard(key)
        self._follow_action_missing_clip_counts.pop(key, None)

    def _shift_follow_actions_after_scene_insert(self, insert_index):
        shifted_rules = {}
        for key, rule in self._follow_action_rules.items():
            target_kind = rule.get("target_kind")
            scene_index = int(rule.get("scene_index", 0))
            if scene_index >= insert_index:
                rule = dict(rule)
                rule["scene_index"] = scene_index + 1
                if target_kind == "clip":
                    key = self._follow_action_key("clip", rule.get("track_index"), rule["scene_index"])
                else:
                    key = self._follow_action_key("scene", None, rule["scene_index"])
            shifted_rules[key] = rule
        self._follow_action_rules = shifted_rules
        self._follow_action_missing_clip_counts = {}

        shifted_active = {}
        for active_key, active in self._active_follow_actions.items():
            if active.get("scene_index") >= insert_index:
                active = dict(active)
                active["scene_index"] += 1
                if active.get("target_kind") == "clip":
                    active_key = self._follow_action_key("clip", active.get("track_index"), active.get("scene_index"))
                else:
                    active_key = self._follow_action_key("scene", None, active.get("scene_index"))
            shifted_active[active_key] = active
        self._active_follow_actions = shifted_active

    def _shift_follow_actions_after_scene_delete(self, deleted_index):
        shifted_rules = {}
        for key, rule in self._follow_action_rules.items():
            scene_index = int(rule.get("scene_index", 0))
            if scene_index == deleted_index:
                continue
            if scene_index > deleted_index:
                rule = dict(rule)
                rule["scene_index"] = scene_index - 1
                if rule.get("target_kind") == "clip":
                    key = self._follow_action_key("clip", rule.get("track_index"), rule["scene_index"])
                else:
                    key = self._follow_action_key("scene", None, rule["scene_index"])
            shifted_rules[key] = rule
        self._follow_action_rules = shifted_rules
        self._follow_action_missing_clip_counts = {}

        shifted_active = {}
        for active_key, active in self._active_follow_actions.items():
            active_scene = active.get("scene_index")
            if active_scene == deleted_index:
                continue
            elif active_scene > deleted_index:
                active = dict(active)
                active["scene_index"] = active_scene - 1
                if active.get("target_kind") == "clip":
                    active_key = self._follow_action_key("clip", active.get("track_index"), active.get("scene_index"))
                else:
                    active_key = self._follow_action_key("scene", None, active.get("scene_index"))
            shifted_active[active_key] = active
        self._active_follow_actions = shifted_active

    def _duplicate_follow_actions_for_scene(self, source_scene_index, dest_scene_index):
        scene_key = self._follow_action_key("scene", None, source_scene_index)
        scene_rule = self._follow_action_rules.get(scene_key)
        if scene_rule:
            new_rule = dict(scene_rule)
            new_rule["scene_index"] = dest_scene_index
            self._follow_action_rules[self._follow_action_key("scene", None, dest_scene_index)] = new_rule
            self._save_follow_action_rule_to_name(new_rule)

        try:
            for track_index, track in enumerate(self.song().tracks):
                self._copy_clip_follow_action_rule(track_index, source_scene_index, track_index, dest_scene_index)
        except Exception:
            pass

    def _reset_follow_action_state(self):
        self._follow_action_rules = {}
        self._active_follow_actions = {}
        self._handled_follow_action_launches = set()
        self._follow_action_missing_clip_counts = {}
        self._last_follow_action_state = None
        self._send_follow_action_state(force=True)

    def _shift_follow_actions_after_track_insert(self, insert_index):
        shifted_rules = {}
        for key, rule in self._follow_action_rules.items():
            if rule.get("target_kind") == "clip":
                track_index = int(rule.get("track_index", 0))
                if track_index >= insert_index:
                    rule = dict(rule)
                    rule["track_index"] = track_index + 1
                key = self._follow_action_key("clip", rule.get("track_index"), rule.get("scene_index"))
            shifted_rules[key] = rule
        self._follow_action_rules = shifted_rules

        shifted_active = {}
        for active_key, active in self._active_follow_actions.items():
            if active.get("target_kind") == "clip":
                track_index = int(active.get("track_index", 0))
                if track_index >= insert_index:
                    active = dict(active)
                    active["track_index"] = track_index + 1
                active_key = self._follow_action_key("clip", active.get("track_index"), active.get("scene_index"))
            shifted_active[active_key] = active
        self._active_follow_actions = shifted_active

        shifted_handled = set()
        for key in self._handled_follow_action_launches:
            if key[0] == "clip":
                _, track_index, scene_index = key
                if track_index >= insert_index:
                    track_index += 1
                shifted_handled.add(self._follow_action_key("clip", track_index, scene_index))
            else:
                shifted_handled.add(key)
        self._handled_follow_action_launches = shifted_handled
        self._follow_action_missing_clip_counts = {}

    def _shift_follow_actions_after_track_delete(self, deleted_index):
        shifted_rules = {}
        for key, rule in self._follow_action_rules.items():
            if rule.get("target_kind") == "clip":
                track_index = int(rule.get("track_index", 0))
                if track_index == deleted_index:
                    continue
                if track_index > deleted_index:
                    rule = dict(rule)
                    rule["track_index"] = track_index - 1
                key = self._follow_action_key("clip", rule.get("track_index"), rule.get("scene_index"))
            shifted_rules[key] = rule
        self._follow_action_rules = shifted_rules

        shifted_active = {}
        for active_key, active in self._active_follow_actions.items():
            if active.get("target_kind") == "clip":
                track_index = int(active.get("track_index", 0))
                if track_index == deleted_index:
                    continue
                if track_index > deleted_index:
                    active = dict(active)
                    active["track_index"] = track_index - 1
                active_key = self._follow_action_key("clip", active.get("track_index"), active.get("scene_index"))
            shifted_active[active_key] = active
        self._active_follow_actions = shifted_active

        shifted_handled = set()
        for key in self._handled_follow_action_launches:
            if key[0] == "clip":
                _, track_index, scene_index = key
                if track_index == deleted_index:
                    continue
                if track_index > deleted_index:
                    track_index -= 1
                shifted_handled.add(self._follow_action_key("clip", track_index, scene_index))
            else:
                shifted_handled.add(key)
        self._handled_follow_action_launches = shifted_handled
        self._follow_action_missing_clip_counts = {}

    def _remap_follow_actions_to_track_signature(self, previous_signature, current_signature):
        current_index_by_track = dict((track_id, index) for index, track_id in enumerate(current_signature))

        remapped_rules = {}
        for key, rule in self._follow_action_rules.items():
            if rule.get("target_kind") == "clip":
                old_track_index = int(rule.get("track_index", 0))
                if old_track_index >= len(previous_signature):
                    continue
                track_id = previous_signature[old_track_index]
                if track_id not in current_index_by_track:
                    continue
                rule = dict(rule)
                rule["track_index"] = current_index_by_track[track_id]
                key = self._follow_action_key("clip", rule.get("track_index"), rule.get("scene_index"))
            remapped_rules[key] = rule
        self._follow_action_rules = remapped_rules

        remapped_active = {}
        for active_key, active in self._active_follow_actions.items():
            if active.get("target_kind") == "clip":
                old_track_index = int(active.get("track_index", 0))
                if old_track_index >= len(previous_signature):
                    continue
                track_id = previous_signature[old_track_index]
                if track_id not in current_index_by_track:
                    continue
                active = dict(active)
                active["track_index"] = current_index_by_track[track_id]
                active_key = self._follow_action_key("clip", active.get("track_index"), active.get("scene_index"))
            remapped_active[active_key] = active
        self._active_follow_actions = remapped_active

        remapped_handled = set()
        for key in self._handled_follow_action_launches:
            if key[0] == "clip":
                _, old_track_index, scene_index = key
                if old_track_index >= len(previous_signature):
                    continue
                track_id = previous_signature[old_track_index]
                if track_id not in current_index_by_track:
                    continue
                remapped_handled.add(self._follow_action_key("clip", current_index_by_track[track_id], scene_index))
            else:
                remapped_handled.add(key)
        self._handled_follow_action_launches = remapped_handled
        self._follow_action_missing_clip_counts = {}

    def _sync_follow_actions_to_track_topology(self):
        current_signature = self._track_signature(self.song().tracks)
        previous_signature = self._follow_action_track_signature
        self._follow_action_track_signature = current_signature

        if previous_signature is None or previous_signature == current_signature:
            return

        if not set(previous_signature).intersection(set(current_signature)):
            self._follow_action_rules = {}
            self._active_follow_actions = {}
            self._handled_follow_action_launches = set()
            self._follow_action_missing_clip_counts = {}
            self._last_follow_action_state = None
            self._load_follow_actions_from_names(force_send=True)
            return

        self._remap_follow_actions_to_track_signature(previous_signature, current_signature)
        self._send_follow_action_state(force=True)

    def _get_global_launch_quantization_beats(self):
        try:
            quantization = self.song().clip_trigger_quantization
        except Exception:
            return 0.0

        quantization_beats = [
            ("q_no_q", 0.0),
            ("q_8_bars", 32.0),
            ("q_4_bars", 16.0),
            ("q_2_bars", 8.0),
            ("q_bar", 4.0),
            ("q_half", 2.0),
            ("q_half_triplet", 4.0 / 3.0),
            ("q_quarter", 1.0),
            ("q_quarter_triplet", 2.0 / 3.0),
            ("q_eight", 0.5),
            ("q_eight_triplet", 1.0 / 3.0),
            ("q_sixteenth", 0.25),
            ("q_sixtenth", 0.25),
            ("q_sixteenth_triplet", 1.0 / 6.0),
            ("q_sixtenth_triplet", 1.0 / 6.0),
            ("q_thirtytwoth", 0.125),
        ]

        for name, beats in quantization_beats:
            try:
                if quantization == getattr(Live.Song.Quantization, name):
                    return beats
            except Exception:
                pass
        return 0.0

    def _clip_slot_length_beats(self, clip_slot):
        try:
            if clip_slot and clip_slot.has_clip:
                clip = clip_slot.clip
                folded_info = self._decoupled_automation_info(clip)
                if folded_info:
                    return max(0.0001, float(folded_info.get("note_length", 0.0001)))
                return max(0.0, float(clip.loop_end) - float(clip.loop_start))
        except Exception:
            pass
        return 0.0

    def _clip_slot_is_triggered(self, clip_slot):
        try:
            return bool(clip_slot and clip_slot.is_triggered)
        except Exception:
            return False

    def _scene_length_beats(self, scene_index):
        longest = 0.0
        try:
            for track in self.song().tracks:
                if scene_index < len(track.clip_slots):
                    longest = max(longest, self._clip_slot_length_beats(track.clip_slots[scene_index]))
        except Exception:
            pass
        return longest

    def _scene_is_playing(self, scene_index):
        try:
            for track in self.song().tracks:
                if scene_index < len(track.clip_slots) and track.clip_slots[scene_index].is_playing:
                    return True
        except Exception:
            pass
        return False

    def _scene_is_triggered(self, scene_index):
        try:
            for track in self.song().tracks:
                if scene_index < len(track.clip_slots) and self._clip_slot_is_triggered(track.clip_slots[scene_index]):
                    return True
        except Exception:
            pass
        return False

    def _scene_object_is_triggered(self, scene):
        try:
            return bool(scene and scene.is_triggered)
        except Exception:
            return False

    def _scene_is_launched(self, scene_index):
        has_clip = False
        try:
            for track in self.song().tracks:
                if scene_index >= len(track.clip_slots):
                    continue
                clip_slot = track.clip_slots[scene_index]
                if not clip_slot.has_clip:
                    continue
                has_clip = True
                if not clip_slot.is_playing and not self._clip_slot_is_triggered(clip_slot):
                    return False
        except Exception:
            return False
        return has_clip

    def _scene_has_clip(self, scene_index):
        try:
            for track in self.song().tracks:
                if scene_index < len(track.clip_slots) and track.clip_slots[scene_index].has_clip:
                    return True
        except Exception:
            pass
        return False

    def _song_is_playing(self):
        try:
            return bool(self.song().is_playing)
        except Exception:
            return False

    def _send_transport_state(self, force=False):
        is_playing = self._song_is_playing()
        value = 127 if is_playing else 0
        if not force and self._last_sent_transport_state == value:
            return
        self._last_sent_transport_state = value
        self.send_cc(118, 0, value)

    def _session_record_value(self):
        try:
            return bool(self.song().session_record)
        except Exception:
            return False

    def _send_session_record_state(self, force=False):
        value = 127 if self._session_record_value() else 0
        if not force and self._last_sent_session_record_state == value:
            return
        self._last_sent_session_record_state = value
        self.send_cc(119, 0, value)

    def _on_session_record_changed(self):
        self._send_session_record_state()

    def _clear_follow_action_launch_state(self):
        changed = bool(self._active_follow_actions or self._handled_follow_action_launches)
        self._active_follow_actions = {}
        self._handled_follow_action_launches = set()
        if changed:
            self._send_follow_action_state()

    def _sync_follow_actions_to_transport(self):
        song_is_playing = self._song_is_playing()
        if song_is_playing != self._last_song_is_playing:
            self._last_song_is_playing = song_is_playing
            if not song_is_playing:
                self._clear_follow_action_launch_state()
                return
            self._clear_finished_follow_action_launches()
            self._activate_follow_actions_for_playing_clips()
            return

    def _on_song_is_playing_changed(self):
        self._send_transport_state()
        self._sync_follow_actions_to_transport()

    def _normalize_follow_action(self, action_name):
        action = str(action_name or "").strip().lower().replace(" ", "_").replace("-", "_")
        aliases = {
            "playagain": "play_again",
            "play_again": "play_again",
            "noaction": "play_again",
            "no_action": "play_again",
            "none": "play_again",
        }
        return aliases.get(action, action)

    def _follow_action_payload_bytes(self, message):
        if len(message) > 3 and message[2] == 1:
            return message[3:-1]
        return message[2:-1]

    def _strip_follow_action_name_marker(self, name):
        return self.FOLLOW_ACTION_NAME_MARKER_RE.sub("", str(name or "")).rstrip()

    def _strip_decoupled_automation_name_marker(self, name):
        return self.DECOUPLED_AUTOMATION_ANY_NAME_MARKER_RE.sub("", str(name or "")).rstrip()

    def _strip_mutator_name_marker(self, name):
        return self.MUTATOR_ANY_NAME_MARKER_RE.sub("", str(name or "")).rstrip()

    def _stable_decoupled_hash(self, value):
        result = 2166136261
        for char in str(value or ""):
            result ^= ord(char)
            result = (result * 16777619) & 0xFFFFFFFF
        return result

    def _decoupled_automation_parameter_key(self, device_param):
        try:
            if device_param is None or not liveobj_valid(device_param):
                return None

            parts = []
            parent = getattr(device_param, "canonical_parent", None)
            while parent is not None and len(parts) < 4:
                name = getattr(parent, "name", None)
                if name:
                    parts.append(str(name))
                next_parent = getattr(parent, "canonical_parent", None)
                if next_parent is parent:
                    break
                parent = next_parent

            parts = list(reversed(parts))
            parts.append(str(getattr(device_param, "name", "parameter")))
            raw_key = "/".join(parts)
            clean = re.sub(r"[^A-Za-z0-9]+", "_", raw_key).strip("_")
            if not clean:
                clean = "parameter"
            return "{}_{}".format(clean[:36], format(self._stable_decoupled_hash(raw_key), "08x"))
        except Exception:
            return None

    def _decode_decoupled_automation_lengths(self, payload):
        lengths = {}
        entries = [entry for entry in str(payload or "").split(",") if entry]
        for entry in entries:
            fields = entry.split(":")
            if len(fields) != 2:
                continue
            try:
                key = fields[0]
                if not key:
                    continue
                lengths[key] = max(0.0001, float(fields[1]))
            except Exception:
                pass
        return lengths

    def _encode_decoupled_automation_lengths(self, lengths):
        entries = []
        for key in sorted((lengths or {}).keys()):
            try:
                length = max(0.0001, float(lengths[key]))
                entries.append("{}:{:.6f}".format(key, length))
            except Exception:
                pass
        return ",".join(entries)

    def _decode_decoupled_automation_payload(self, payload):
        fields = str(payload or "").split("|")
        if len(fields) != 3:
            return None

        try:
            note_start = max(0.0, float(fields[0]))
            note_length = max(0.0001, float(fields[1]))
            automation_lengths = self._decode_decoupled_automation_lengths(fields[2])
        except Exception:
            return None

        if not automation_lengths:
            return None

        physical_length = self._decoupled_physical_length(note_length, automation_lengths.values())
        return {
            "note_start": note_start,
            "note_length": note_length,
            "note_end": note_start + note_length,
            "automation_lengths": automation_lengths,
            "physical_length": physical_length,
            "physical_end": note_start + physical_length,
        }

    def _decoupled_automation_info_from_name(self, name):
        matches = self.DECOUPLED_AUTOMATION_NAME_MARKER_RE.findall(str(name or ""))
        if not matches:
            return None
        return self._decode_decoupled_automation_payload(matches[-1])

    def _decoupled_automation_info(self, clip, device_param=None):
        try:
            if clip is None or not hasattr(clip, "name"):
                return None
            info = self._decoupled_automation_info_from_name(clip.name)
            if not info:
                return None
            info = dict(info)
            max_physical_length = self._decoupled_automation_max_physical_length(clip, info["note_length"])
            info["physical_length"] = self._decoupled_physical_length(info["note_length"], info["automation_lengths"].values(), max_physical_length)
            info["physical_end"] = info["note_start"] + info["physical_length"]
            parameter_key = self._decoupled_automation_parameter_key(device_param)
            if parameter_key:
                info["parameter_key"] = parameter_key
                info["has_parameter_length"] = parameter_key in info["automation_lengths"]
                info["automation_length"] = info["automation_lengths"].get(parameter_key, info["note_length"])
            else:
                info["parameter_key"] = None
                info["has_parameter_length"] = False
                info["automation_length"] = info["note_length"]
            return info
        except Exception:
            return None

    def _decoupled_automation_marker(self, info):
        return "[TapAuto:v2|{:.6f}|{:.6f}|{}]".format(
            info.get("note_start", 0.0),
            info.get("note_length", 0.0),
            self._encode_decoupled_automation_lengths(info.get("automation_lengths", {})),
        )

    def _save_decoupled_automation_info_to_name(self, clip, info):
        if clip is None or not hasattr(clip, "name"):
            return

        try:
            clean_name = self._strip_decoupled_automation_name_marker(clip.name)
            marker = self._decoupled_automation_marker(info)
            new_name = "{} {}".format(clean_name, marker).strip() if clean_name else marker
            if clip.name != new_name:
                clip.name = new_name
        except Exception:
            pass

    def _remove_decoupled_automation_info_from_name(self, clip):
        if clip is None or not hasattr(clip, "name"):
            return

        try:
            clean_name = self._strip_decoupled_automation_name_marker(clip.name)
            if clip.name != clean_name:
                clip.name = clean_name
        except Exception:
            pass

    def _mutator_info_from_name(self, name, resolve_scale_root=True):
        try:
            matches = self.MUTATOR_NAME_MARKER_RE.findall(str(name or ""))
            if not matches:
                return None
            payload = matches[-1]
            fields = {}
            for item in payload.split("|"):
                if "=" not in item:
                    continue
                key, value = item.split("=", 1)
                fields[key] = value
            sections = []
            for section in fields.get("sec", "").split(","):
                if not section:
                    continue
                parts = section.split(":")
                if len(parts) >= 3:
                    sections.append({
                        "role": int(parts[0]),
                        "start": float(parts[1]),
                        "length": float(parts[2]),
                    })
            algorithm = fields.get("alg", "")
            if fields.get("ai", "").isdigit():
                algorithm = self._mutator_algorithm_from_code(int(fields.get("ai", "0")))

            scale_index = self._mutator_scale_index_from_name(fields.get("sc", ""))
            if fields.get("si", "").isdigit():
                scale_index = max(0, min(len(self.MUTATOR_SCALE_NAMES) - 1, int(fields.get("si", "2"))))
            if scale_index is None:
                scale_index = 2
            depth = 0.0
            try:
                if "dep" in fields:
                    depth = float(fields.get("dep", "0.0"))
                elif "dp" in fields:
                    depth = float(fields.get("dp", "0")) / 100.0
            except Exception:
                depth = 0.0

            preset = int(fields.get("pr", "9"))
            settings_preset = int(fields.get("sp", fields.get("pr", "9")))
            pending_settings_update = int(fields.get("pd", "0")) == 1

            companion_mode = self._mutator_companion_mode_from_code(int(fields.get("cm", "0"))) if fields.get("cm", "").isdigit() else "melody"
            target_pitches = self._mutator_target_pitches_from_value(fields.get("tg", ""))
            operation_depths = self._mutator_operation_depths_from_value(
                fields.get("od", ""),
                self._mutator_default_operation_depths()
            )
            operation_order = self._mutator_operation_order_from_value(fields.get("oo", ""))
            mutator_slots = self._mutator_slots_from_value(fields.get("ms", ""))
            mutator_slot_count = self._mutator_slot_count_from_value(fields.get("mc", ""), mutator_slots)

            info = {
                "original_loop_length": max(0.0001, float(fields.get("ol", "0.0001"))),
                "structure_length": max(0.0001, float(fields.get("sl", fields.get("ol", "0.0001")))),
                "preset": preset,
                "settings_preset": settings_preset,
                "mutations_per_pass": max(1, min(3, int(fields.get("mp", "1")))),
                "seed": int(fields.get("seed", fields.get("sd", "1"))),
                "algorithm": algorithm or "mutator",
                "algorithm_code": self._mutator_algorithm_code(algorithm or "mutator"),
                "depth": max(0.0, min(1.0, depth)),
                "regenerate_mode": int(fields.get("rg", "0")),
                "source_mode": int(fields.get("src", "2")),
                "root": max(0, min(11, int(fields.get("rt", "0")))),
                "scale_index": scale_index,
                "companion_mode": companion_mode,
                "companion_mode_code": self._mutator_companion_mode_code(companion_mode),
                "target_pitches": target_pitches,
                "operation_order": operation_order,
                "mutator_slots": mutator_slots,
                "mutator_slot_count": mutator_slot_count,
                "pending_settings_update": pending_settings_update,
                "sections": sections,
            }
            info.update(operation_depths)
            if resolve_scale_root:
                live_scale_index, live_root = self._current_mutator_scale_root(
                    info.get("scale_index", 2),
                    info.get("root", 0)
                )
                info["scale_index"] = live_scale_index
                info["root"] = live_root
            return info
        except Exception:
            return None

    def _mutator_info(self, clip):
        if clip is None or not hasattr(clip, "name"):
            return None
        info = self._mutator_info_from_name(clip.name)
        if not info:
            return None
        if not info.get("sections"):
            original_loop_length = max(0.0001, float(info.get("original_loop_length", 0.0001)))
            source_start = float(getattr(clip, "loop_start", 0.0))
            roles = self._mutator_pattern_roles(info.get("preset", 9))
            info["sections"] = [
                {"role": role, "start": source_start + (index * original_loop_length), "length": original_loop_length}
                for index, role in enumerate(roles)
            ]
            info["structure_length"] = max(
                original_loop_length,
                float(info.get("structure_length", 0.0)) or (original_loop_length * len(roles))
            )
        return info

    def _mutator_source_note_range(self, clip):
        info = self._mutator_info(clip)
        if not info:
            return None
        decoupled_info = self._decoupled_automation_info(clip)
        source_start = decoupled_info["note_start"] if decoupled_info else float(getattr(clip, "loop_start", 0.0))
        original_loop_length = max(
            0.0001,
            float(info.get("original_loop_length", float(getattr(clip, "loop_end", source_start)) - source_start))
        )
        return source_start, source_start + original_loop_length

    def _mutator_allows_source_note_time(self, clip, start_time):
        source_range = self._mutator_source_note_range(clip)
        if not source_range:
            return True
        source_start, source_end = source_range
        return float(start_time) >= source_start - 0.000001 and float(start_time) < source_end - 0.000001

    def _mutator_algorithm_code(self, algorithm):
        try:
            return self.MUTATOR_ALGORITHMS.index(str(algorithm or "mutator"))
        except Exception:
            return 0

    def _mutator_algorithm_from_code(self, code):
        try:
            index = max(0, min(len(self.MUTATOR_ALGORITHMS) - 1, int(code)))
            return self.MUTATOR_ALGORITHMS[index]
        except Exception:
            return "mutator"

    def _mutator_companion_mode_code(self, mode):
        return 1 if str(mode or "melody") == "rhythm" else 0

    def _mutator_companion_mode_from_code(self, code):
        return "rhythm" if int(code) == 1 else "melody"

    def _mutator_scale_index_from_name(self, scale_name):
        try:
            return self.MUTATOR_SCALE_NAMES.index(str(scale_name))
        except Exception:
            return None

    def _current_mutator_scale_root(self, fallback_scale_index=2, fallback_root=0):
        scale_index = fallback_scale_index
        root = fallback_root
        try:
            song = self.song()
            live_scale_index = self._mutator_scale_index_from_name(getattr(song, "scale_name", ""))
            if live_scale_index is not None:
                scale_index = live_scale_index
            root = max(0, min(11, int(getattr(song, "root_note", fallback_root))))
        except Exception:
            pass
        return scale_index, root

    def _current_mutator_scale_root_signature(self):
        return self._current_mutator_scale_root(2, 0)

    def _mutator_operation_is_scale_sensitive(self, operation):
        try:
            return int(operation) in (
                6,
                self._mutator_add_shift_operation_index(),
                self._mutator_invert_operation_index(),
                self._mutator_pitch_add_operation_index(),
                self._mutator_phrase_shift_operation_index(),
            )
        except Exception:
            return False

    def _mutator_info_is_scale_sensitive(self, info):
        if not info or info.get("companion_mode", "melody") == "rhythm":
            return False
        if self._mutator_algorithm_kind(info.get("algorithm", "mutator")) is not None:
            return True
        if self._mutator_depth_value(info, "pitch_shift_depth", 0.0) > 0.0:
            return True
        for slot in self._mutator_visible_slots(info):
            if (
                slot
                and self._mutator_operation_is_scale_sensitive(slot.get("operation", 0))
                and self._mutator_depth_value(slot, "probability_depth", 0.0) > 0.0
            ):
                return True
        return False

    def _mutator_clip_slots(self):
        try:
            for track_index, track in enumerate(self.song().tracks):
                if track is None or not hasattr(track, "clip_slots"):
                    continue
                for scene_index, clip_slot in enumerate(track.clip_slots):
                    if clip_slot is not None and clip_slot.has_clip:
                        yield track_index, scene_index, clip_slot
        except Exception:
            return

    def _sync_mutator_scale_root_to_companion_clips(self):
        self._mutator_scale_root_sync_scheduled = False
        try:
            scale_index, root = self._current_mutator_scale_root_signature()
            signature = (scale_index, root)
            previous_signature = self._last_mutator_scale_root_signature
            self._last_mutator_scale_root_signature = signature
            if previous_signature is None or previous_signature == signature:
                return

            for _, _, clip_slot in self._mutator_clip_slots():
                clip = clip_slot.clip
                raw_info = self._mutator_info_from_name(clip.name, resolve_scale_root=False)
                if not raw_info:
                    continue
                info = dict(raw_info)
                scale_root_changed = (
                    int(info.get("scale_index", 2)) != scale_index
                    or int(info.get("root", 0)) != root
                )
                info["scale_index"] = scale_index
                info["root"] = root
                if scale_root_changed:
                    self._save_mutator_info_to_name(clip, info)
                if not self._mutator_info_is_scale_sensitive(info):
                    continue

                key = self._live_object_identity(clip)
                settings = self._mutator_settings_from_info(
                    info,
                    seed=random.randint(1, 2000000000)
                )
                if self._mutator_generation_is_busy(key):
                    self._queue_mutator_generation(key, clip, settings, previous_info=info, send_updates=True)
                elif not self._schedule_mutator_generation(key, clip, settings, previous_info=info, send_updates=True):
                    self._generate_mutator_clip(clip, settings, previous_info=info, send_updates=True)
        except Exception as e:
            self._debug_log("Error syncing mutator scale/root: {}".format(str(e)))

    def _schedule_mutator_scale_root_sync(self):
        if self._mutator_scale_root_sync_scheduled:
            return
        self._mutator_scale_root_sync_scheduled = True
        try:
            self.schedule_message(1, self._sync_mutator_scale_root_to_companion_clips)
        except Exception:
            self._sync_mutator_scale_root_to_companion_clips()

    def _mutator_depth_value(self, settings, key, fallback=0.0):
        try:
            return max(0.0, min(1.0, float(settings.get(key, fallback))))
        except Exception:
            return max(0.0, min(1.0, float(fallback)))

    def _mutator_operation_depth_keys(self):
        return (
            "fill_depth",
            "simplification_depth",
            "octave_shift_depth",
            "rhythmic_shift_depth",
            "note_addition_depth",
            "note_removal_depth",
            "pitch_shift_depth",
            "velocity_change_depth",
            "gate_change_depth",
            "shift_depth",
        )

    def _mutator_add_shift_operation_index(self):
        return len(self._mutator_operation_depth_keys())

    def _mutator_loop_shift_operation_index(self):
        return 11

    def _mutator_reverse_operation_index(self):
        return 12

    def _mutator_invert_operation_index(self):
        return 13

    def _mutator_pitch_add_operation_index(self):
        return 14

    def _mutator_duplicate_operation_index(self):
        return 15

    def _mutator_phrase_shift_operation_index(self):
        return 16

    def _mutator_preserver_operation_index(self):
        return 17

    def _mutator_max_operation_index(self):
        return self._mutator_preserver_operation_index()

    def _mutator_default_operation_depths(self):
        return dict((key, 0.0) for key in self._mutator_operation_depth_keys())

    def _mutator_operation_depths_from_fields(self, fields, start_index, fallback):
        result = dict(fallback or {})
        keys = self._mutator_operation_depth_keys()
        for offset, key in enumerate(keys):
            try:
                index = int(start_index) + offset
                if index < len(fields):
                    result[key] = max(0.0, min(1.0, float(fields[index]) / 100.0))
            except Exception:
                pass
        return result

    def _mutator_operation_depths_from_value(self, value, fallback):
        result = dict(fallback or {})
        parts = str(value or "").split(",")
        for offset, key in enumerate(self._mutator_operation_depth_keys()):
            try:
                if offset < len(parts) and parts[offset] != "":
                    result[key] = max(0.0, min(1.0, float(parts[offset]) / 100.0))
            except Exception:
                pass
        return result

    def _mutator_operation_depths_marker_value(self, info):
        return ",".join(
            str(max(0, min(100, int(round(self._mutator_depth_value(info, key, 0.0) * 100.0)))))
            for key in self._mutator_operation_depth_keys()
        )

    def _mutator_operation_order_from_value(self, value, fallback=None):
        keys = self._mutator_operation_depth_keys()
        result = []
        parts = str(value or "").split(",")
        for part in parts:
            try:
                if part == "":
                    continue
                index = int(part)
                if 0 <= index < len(keys) and index not in result:
                    result.append(index)
            except Exception:
                pass
        if result:
            return result
        return list(fallback or ())

    def _mutator_active_operation_order(self, settings):
        keys = self._mutator_operation_depth_keys()
        default_order = (0, 1, 3, 9, 4, 8, 2, 6, 5, 7)
        stored_order = settings.get("operation_order", [])
        if isinstance(stored_order, (list, tuple)):
            stored_order = ",".join(str(index) for index in stored_order)
        raw_order = self._mutator_operation_order_from_value(
            stored_order,
            default_order
        )
        result = []
        for index in list(raw_order) + list(default_order):
            if 0 <= int(index) < len(keys) and int(index) not in result:
                key = keys[int(index)]
                if self._mutator_depth_value(settings, key, 0.0) > 0.0:
                    result.append(int(index))
        return result

    def _mutator_operation_order_marker_value(self, info):
        return ",".join(str(slot.get("operation", 0)) for slot in self._mutator_active_slots(info))

    def _mutator_slots_from_value(self, value):
        result = []
        max_operation_index = self._mutator_max_operation_index()
        for part in str(value or "").split(";"):
            if len(result) >= 10:
                break
            if not part or part == "x":
                result.append(None)
                continue
            fields = part.split(",")
            try:
                operation = int(fields[0])
                if operation < 0 or operation > max_operation_index:
                    result.append(None)
                    continue
                activation = max(0.0, min(1.0, float(fields[1]) / 100.0 if len(fields) > 1 else 1.0))
                probability = max(0.0, min(1.0, float(fields[2]) / 100.0 if len(fields) > 2 else 0.0))
                strength = max(0.0, min(1.0, float(fields[3]) / 100.0 if len(fields) > 3 else 0.5))
                result.append(dict(
                    operation=operation,
                    activation_probability=activation,
                    probability_depth=probability,
                    range_depth=strength,
                ))
            except Exception:
                result.append(None)
        while len(result) < 10:
            result.append(None)
        return result[:10]

    def _mutator_slots_marker_value(self, info):
        parts = []
        for slot in self._mutator_visible_slots(info):
            if not slot:
                parts.append("x")
                continue
            operation = max(0, min(self._mutator_max_operation_index(), int(slot.get("operation", 0))))
            parts.append("{},{},{},{}".format(
                operation,
                max(0, min(100, int(round(self._mutator_depth_value(slot, "activation_probability", 1.0) * 100.0)))),
                max(0, min(100, int(round(self._mutator_depth_value(slot, "probability_depth", 0.0) * 100.0)))),
                max(0, min(100, int(round(self._mutator_depth_value(slot, "range_depth", 0.5) * 100.0)))),
            ))
        return ";".join(parts)

    def _mutator_operation_uses_range_depth(self, operation):
        return int(operation) in (3, 6, 7, 8, 9, self._mutator_pitch_add_operation_index(), self._mutator_phrase_shift_operation_index())

    def _mutator_normalized_slots(self, settings):
        slots = settings.get("mutator_slots", [])
        result = []
        max_operation_index = self._mutator_max_operation_index()
        if isinstance(slots, (list, tuple)):
            for slot in slots[:10]:
                if not slot:
                    result.append(None)
                    continue
                try:
                    operation = int(slot.get("operation", 0))
                    if operation < 0 or operation > max_operation_index:
                        result.append(None)
                        continue
                    result.append(dict(
                        operation=operation,
                        activation_probability=self._mutator_depth_value(slot, "activation_probability", 1.0),
                        probability_depth=self._mutator_depth_value(slot, "probability_depth", 0.0),
                        range_depth=self._mutator_depth_value(slot, "range_depth", 0.5),
                    ))
                except Exception:
                    result.append(None)
        while len(result) < 10:
            result.append(None)
        return result[:10]

    def _mutator_slot_count_from_value(self, value, slots=None):
        try:
            if str(value or "") != "":
                return max(4, min(10, int(value)))
        except Exception:
            pass
        last_used = 0
        for index, slot in enumerate(tuple(slots or ())[:10]):
            if slot:
                last_used = index + 1
        return max(4, min(10, last_used))

    def _mutator_visible_slots(self, settings):
        slots = self._mutator_normalized_slots(settings)
        slot_count = self._mutator_slot_count_from_value(settings.get("mutator_slot_count", ""), slots)
        return slots[:slot_count]

    def _mutator_resolve_slot_activation(self, settings):
        resolved = dict(settings or {})
        rnd = random.Random(int(resolved.get("seed", 1)) ^ 0x5A175EED)
        active_slots = []
        for slot in self._mutator_visible_slots(resolved):
            if not slot:
                continue
            activation = self._mutator_depth_value(slot, "activation_probability", 1.0)
            if activation <= 0.0:
                continue
            if activation < 1.0 and rnd.random() > activation:
                continue
            active_slots.append(slot)
        resolved["_active_mutator_slots"] = active_slots
        return resolved

    def _mutator_active_slots(self, settings):
        active = []
        add_shift_index = self._mutator_add_shift_operation_index()
        source_slots = settings.get("_active_mutator_slots", self._mutator_visible_slots(settings))
        for slot in source_slots:
            if not slot:
                continue
            probability = self._mutator_depth_value(slot, "probability_depth", 0.0)
            if probability <= 0.0:
                continue
            operation = int(slot.get("operation", 0))
            if operation == add_shift_index:
                active.append(dict(operation=operation, probability_depth=probability, range_depth=probability))
            else:
                active.append(slot)
        return active

    def _mutator_role_operation_depth(self, role, operation_depth):
        amount = max(0.0, min(1.0, float(operation_depth)))
        if amount <= 0.0:
            return 0.0
        role_factors = {
            0: 0.0,
            1: 0.35,
            2: 0.58,
            3: 0.82,
            4: 0.70,
            5: 1.0,
            6: 1.0,
            7: 1.0,
            8: 0.0,
            9: 1.0,
            10: 1.0,
            11: 1.0,
            12: 0.65,
            13: 0.50,
            14: 1.0,
            15: 1.0,
            16: 1.0,
        }
        return min(1.0, role_factors.get(role, 1.0) * amount)

    def _mutator_target_pitches_from_value(self, value):
        pitches = []
        try:
            for part in str(value or "").split(","):
                if part == "":
                    continue
                pitch = max(0, min(127, int(part)))
                if pitch not in pitches:
                    pitches.append(pitch)
        except Exception:
            return []
        return pitches[:16]

    def _mutator_marker(self, info):
        target_pitches = ",".join(str(max(0, min(127, int(pitch)))) for pitch in info.get("target_pitches", [])[:16])
        return "[TapComp:v1|ol={:.4f}|pr={}|sp={}|mp={}|pd={}|sl={:.4f}|ai={}|dp={}|rg={}|src={}|si={}|rt={}|cm={}|tg={}|od={}|oo={}|ms={}|mc={}]".format(
            info.get("original_loop_length", 0.0001),
            info.get("preset", 9),
            info.get("settings_preset", info.get("preset", 9)),
            int(info.get("mutations_per_pass", 1)) & 0x7F,
            1 if info.get("pending_settings_update", False) else 0,
            info.get("structure_length", info.get("original_loop_length", 0.0001)),
            int(info.get("algorithm_code", self._mutator_algorithm_code(info.get("algorithm", "mutator")))) & 0x7F,
            max(0, min(100, int(round(float(info.get("depth", 0.0)) * 100.0)))),
            int(info.get("regenerate_mode", 0)) & 0x7F,
            int(info.get("source_mode", 2)) & 0x7F,
            int(info.get("scale_index", 2)) & 0x7F,
            int(info.get("root", 0)) & 0x7F,
            int(info.get("companion_mode_code", self._mutator_companion_mode_code(info.get("companion_mode", "melody")))) & 0x7F,
            target_pitches,
            self._mutator_operation_depths_marker_value(info),
            self._mutator_operation_order_marker_value(info),
            self._mutator_slots_marker_value(info),
            self._mutator_slot_count_from_value(info.get("mutator_slot_count", ""), self._mutator_normalized_slots(info)),
        )

    def _save_mutator_info_to_name(self, clip, info):
        if clip is None or not hasattr(clip, "name"):
            return
        try:
            clean_name = self._strip_mutator_name_marker(clip.name)
            marker = self._mutator_marker(info)
            new_name = "{} {}".format(clean_name, marker).strip() if clean_name else marker
            if clip.name != new_name:
                clip.name = new_name
        except Exception:
            pass

    def _remove_mutator_info_from_name(self, clip):
        if clip is None or not hasattr(clip, "name"):
            return
        try:
            clean_name = self._strip_mutator_name_marker(clip.name)
            if clip.name != clean_name:
                clip.name = clean_name
        except Exception:
            pass

    def _decoupled_automation_max_physical_length(self, clip=None, note_length=0.0001):
        try:
            numerator = int(getattr(clip, "signature_numerator", 4)) if clip is not None else 4
        except Exception:
            numerator = 4
        return max(float(note_length or 0.0001), float(max(1, numerator) * self.DECOUPLED_AUTOMATION_MAX_PHYSICAL_BARS))

    def _smallest_shared_loop_length(self, note_length, automation_length, max_length=None):
        try:
            note_ms = max(1, int(round(float(note_length) * 1000.0)))
            automation_ms = max(1, int(round(float(automation_length) * 1000.0)))
            shared_ms = (note_ms * automation_ms) // math.gcd(note_ms, automation_ms)
            if max_length is not None:
                max_ms = max(1, int(round(float(max_length) * 1000.0)))
                shared_ms = min(shared_ms, max_ms)
            return float(shared_ms) / 1000.0
        except Exception:
            fallback = max(float(note_length or 0.0), float(automation_length or 0.0), 0.0001)
            return min(fallback, max_length) if max_length is not None else fallback

    def _decoupled_physical_length(self, note_length, automation_lengths, max_length=None):
        lengths = [max(0.0001, float(note_length or 0.0001))]
        for length in automation_lengths or ():
            try:
                lengths.append(max(0.0001, float(length)))
            except Exception:
                pass

        if max_length is None:
            max_length = self._decoupled_automation_max_physical_length(None, note_length)

        try:
            shared_ms = max(1, int(round(lengths[0] * 1000.0)))
            max_ms = max(1, int(round(float(max_length) * 1000.0)))
            for length in lengths[1:]:
                length_ms = max(1, int(round(length * 1000.0)))
                shared_ms = (shared_ms * length_ms) // math.gcd(shared_ms, length_ms)
                if shared_ms >= max_ms:
                    return float(max_ms) / 1000.0
            return min(float(shared_ms) / 1000.0, float(max_length))
        except Exception:
            return min(max(lengths), float(max_length))

    def _positive_mod(self, value, length):
        if length <= 0.000001:
            return 0.0
        return value - (math.floor(value / length) * length)

    def _folded_note_time(self, time_value, info):
        return info["note_start"] + self._positive_mod(float(time_value) - info["note_start"], info["note_length"])

    def _note_matches_folded_time(self, note, folded_time, info, pitch=None, epsilon=0.0005):
        try:
            if pitch is not None and int(note.pitch) != int(pitch):
                return False
            return abs(self._folded_note_time(note.start_time, info) - folded_time) <= epsilon
        except Exception:
            return False

    def _repeat_count_for_decoupled_info(self, info):
        return max(1, int(round(info["physical_length"] / info["note_length"])))

    def _duration_inside_note_loop(self, start_time, duration, info):
        base_offset = self._positive_mod(float(start_time) - info["note_start"], info["note_length"])
        remaining = max(0.0001, info["note_length"] - base_offset)
        return max(0.0001, min(float(duration), remaining))

    def _make_repeated_note_specs_from_values(self, pitch, start_time, duration, velocity, mute, probability, info):
        specs = []
        base_offset = self._positive_mod(float(start_time) - info["note_start"], info["note_length"])
        clipped_duration = self._duration_inside_note_loop(start_time, duration, info)
        for repeat_index in range(self._repeat_count_for_decoupled_info(info)):
            start_time = info["note_start"] + (float(repeat_index) * info["note_length"]) + base_offset
            if start_time < info["physical_end"] - 0.000001:
                specs.append(MidiNoteSpecification(
                    pitch=int(pitch),
                    start_time=start_time,
                    duration=clipped_duration,
                    velocity=int(velocity),
                    mute=bool(mute),
                    probability=float(probability)
                ))
        return specs

    def _make_repeated_note_specs(self, base_note, info):
        return self._make_repeated_note_specs_from_values(
            getattr(base_note, "pitch", 0),
            getattr(base_note, "start_time", info["note_start"]),
            getattr(base_note, "duration", 0.0001),
            getattr(base_note, "velocity", 100),
            getattr(base_note, "mute", False),
            getattr(base_note, "probability", 1.0),
            info
        )

    def _rewrite_decoupled_note_copies(self, clip, info):
        try:
            base_notes = clip.get_notes_extended(0, 128, info["note_start"], info["note_length"])
            if hasattr(clip, "remove_notes_extended"):
                remove_start = float(info.get("remove_start", info["note_start"]))
                current_end = max(
                    info["physical_end"],
                    float(getattr(clip, "loop_end", info["physical_end"])),
                    float(getattr(clip, "end_marker", info["physical_end"])),
                    float(getattr(clip, "length", info["physical_length"])),
                )
                clip.remove_notes_extended(0, 128, remove_start, max(info["physical_length"], current_end - remove_start))
            specs = []
            for note in base_notes:
                specs.extend(self._make_repeated_note_specs(note, info))
            if specs:
                clip.add_new_notes(specs)
        except Exception as e:
            self._debug_log("Error rewriting decoupled note copies: {}".format(str(e)))

    def _expanded_decoupled_automation_steps(self, info, logical_steps, sample_duration):
        if not info or not logical_steps:
            return tuple(logical_steps or ())

        expanded_steps = []
        loop_start_value = logical_steps[0][2]
        repeat_count = max(1, int(round(info["physical_length"] / info["automation_length"])))
        for repeat_index in range(repeat_count):
            cycle_start = info["note_start"] + (float(repeat_index) * info["automation_length"])
            for step in logical_steps:
                time_value, duration, normalized, curve, step_id, step_order = self._automation_step_tuple(step)
                if time_value >= info["note_start"] + info["automation_length"] - 0.000001:
                    relative_time = info["automation_length"]
                else:
                    relative_time = self._positive_mod(time_value - info["note_start"], info["automation_length"])
                if repeat_index > 0 and relative_time <= 0.000001:
                    continue
                expanded_time = cycle_start + relative_time
                if expanded_time <= info["physical_end"] + 0.000001:
                    expanded_steps.append((expanded_time, duration, normalized, curve, step_id, step_order))
            cycle_end = cycle_start + info["automation_length"]
            if cycle_end <= info["physical_end"] + 0.000001:
                expanded_steps.append((
                    min(cycle_end, info["physical_end"]),
                    sample_duration,
                    loop_start_value,
                    0.0,
                    0,
                    self.AUTOMATION_FOLDED_ENDPOINT_ORDER
                ))
        return self._automation_sorted_steps(expanded_steps)

    def _neutralize_decoupled_automation_points(self, envelope, device_param, info, previous_logical_steps, normalized_value, sample_duration):
        if envelope is None or device_param is None or not info:
            return

        raw_value = self._parameter_target_value_from_normalized(device_param, normalized_value)
        minimum_duration = 0.0001
        start_time = info["note_start"]
        try:
            envelope.insert_step(start_time, max(minimum_duration, info["physical_end"] - start_time), raw_value)
        except Exception:
            pass

        previous_physical_steps = self._expanded_decoupled_automation_steps(info, previous_logical_steps or (), sample_duration)
        seen_times = set()
        for step in previous_physical_steps:
            time_value = max(0.0, min(info["physical_end"], step[0]))
            time_key = int(round(time_value * 1000000.0))
            if time_key in seen_times:
                continue
            seen_times.add(time_key)
            try:
                envelope.insert_step(time_value, max(minimum_duration, sample_duration), raw_value)
            except Exception:
                pass

    def _automation_steps_from_envelope_samples(self, envelope, device_param, start, length, sample_duration):
        if envelope is None or device_param is None or not liveobj_valid(device_param):
            return tuple()

        samples = []
        count = max(2, min(self.AUTOMATION_ENVELOPE_MAX_SAMPLES, int(math.ceil(float(length) / sample_duration)) + 1))
        if count > 1:
            sample_duration = max(0.0001, float(length) / float(count - 1))
        for index in range(count):
            time_value = start + (float(index) * sample_duration)
            try:
                raw_value = envelope.value_at_time(time_value)
                if device_param.max != device_param.min:
                    normalized = (raw_value - device_param.min) / (device_param.max - device_param.min)
                else:
                    normalized = self._parameter_normalized_value(device_param)
            except Exception:
                normalized = self._parameter_normalized_value(device_param)
            samples.append((time_value, max(0.0, min(1.0, normalized))))

        return self._automation_sorted_steps(
            (time_value, sample_duration, normalized, 0.0, 0, 0)
            for time_value, normalized in self._compress_automation_samples(samples)
        )

    def _sampled_decoupled_automation_write_steps(self, info, logical_steps, sample_duration):
        expanded_steps = list(self._expanded_decoupled_automation_steps(info, logical_steps, sample_duration))
        if not expanded_steps:
            return tuple()

        clip_end = info["physical_end"]
        boundary_start = info["note_start"]
        guard_duration = 0.0001
        all_steps = []

        def append_target_step(time_value, duration, normalized, force=False):
            if time_value < -0.000001 or time_value > clip_end + 0.000001:
                return

            time_value = max(0.0, min(clip_end, time_value))
            normalized = max(0.0, min(1.0, normalized))
            if all_steps and not force:
                previous_time, _, previous_value, _ = all_steps[-1]
                if abs(previous_time - time_value) <= 0.000001 and abs(previous_value - normalized) <= self.AUTOMATION_ENVELOPE_LINEAR_EPSILON:
                    return

            all_steps.append((time_value, max(guard_duration, duration), normalized, bool(force)))

        if len(expanded_steps) == 1:
            step = expanded_steps[0]
            append_target_step(step[0], step[1], step[2])
        else:
            for index in range(len(expanded_steps) - 1):
                start_step = expanded_steps[index]
                next_step = expanded_steps[index + 1]
                append_target_step(start_step[0], start_step[1], start_step[2])

                time_value = start_step[0] + sample_duration
                while time_value < next_step[0] - 0.000001:
                    append_target_step(
                        time_value,
                        sample_duration,
                        self._automation_value_from_steps(time_value, expanded_steps)
                    )
                    time_value += sample_duration

            last_step = expanded_steps[-1]
            append_target_step(last_step[0], last_step[1], last_step[2])

        first_step = expanded_steps[0]
        if first_step[0] > boundary_start + guard_duration:
            append_target_step(boundary_start, max(guard_duration, first_step[0] - boundary_start), first_step[2], force=True)

        last_step = expanded_steps[-1]
        if clip_end > last_step[0] + guard_duration:
            append_target_step(clip_end - guard_duration, guard_duration, last_step[2], force=True)

        all_steps.sort(key=lambda item: item[0])
        coalesced_steps = []
        for step in all_steps:
            if not coalesced_steps:
                coalesced_steps.append(step)
                continue

            previous_time, _, previous_value, previous_force = coalesced_steps[-1]
            time_value, _, normalized, force = step
            if abs(previous_time - time_value) <= 0.000001 and abs(previous_value - normalized) <= self.AUTOMATION_ENVELOPE_LINEAR_EPSILON:
                if force and not previous_force:
                    coalesced_steps[-1] = step
                continue
            if not force and not previous_force and abs(previous_time - time_value) > 0.000001 and abs(previous_value - normalized) <= self.AUTOMATION_ENVELOPE_LINEAR_EPSILON:
                continue

            coalesced_steps.append(step)

        return tuple(coalesced_steps)

    def _decoupled_info_for_parameter_key(self, info, parameter_key):
        parameter_info = dict(info)
        parameter_info["parameter_key"] = parameter_key
        parameter_info["has_parameter_length"] = parameter_key in info.get("automation_lengths", {})
        parameter_info["automation_length"] = info.get("automation_lengths", {}).get(parameter_key, info["note_length"])
        return parameter_info

    def _rewrite_decoupled_automation_for_parameter(self, clip, device_param, control_index, info, sample_duration):
        if clip is None or device_param is None or not liveobj_valid(device_param) or not info:
            return

        parameter_key = self._decoupled_automation_parameter_key(device_param)
        if not parameter_key:
            return

        parameter_info = self._decoupled_info_for_parameter_key(info, parameter_key)
        envelope = None
        if hasattr(clip, "automation_envelope"):
            try:
                envelope = clip.automation_envelope(device_param)
            except Exception:
                envelope = None

        authored_steps = self._authored_automation_steps(clip, device_param, control_index)
        if authored_steps is not None:
            logical_steps = self._normalize_decoupled_logical_automation_steps(parameter_info, authored_steps)
        elif envelope is not None:
            logical_steps = self._automation_steps_from_envelope_samples(
                envelope,
                device_param,
                parameter_info["note_start"],
                parameter_info["automation_length"],
                sample_duration
            )
        else:
            return

        if not logical_steps:
            return

        if envelope is None and hasattr(clip, "create_automation_envelope"):
            try:
                envelope = clip.create_automation_envelope(device_param)
            except Exception:
                envelope = None
        if envelope is None:
            return

        write_steps = self._sampled_decoupled_automation_write_steps(parameter_info, logical_steps, sample_duration)
        if not write_steps:
            return

        self._neutralize_decoupled_automation_points(
            envelope,
            device_param,
            parameter_info,
            authored_steps or logical_steps,
            write_steps[0][2],
            sample_duration
        )

        self._write_automation_steps_to_envelope(envelope, device_param, parameter_info, write_steps)
        self._store_authored_automation_steps(clip, device_param, control_index, logical_steps)

    def _write_automation_steps_to_envelope(self, envelope, device_param, info, write_steps):
        if envelope is None or device_param is None or not info or not write_steps:
            return

        minimum_duration = 0.0001
        previous_insert_time = None
        for index, step in enumerate(write_steps):
            time_value, duration, normalized, _ = step
            raw_value = self._parameter_target_value_from_normalized(device_param, normalized)
            if previous_insert_time is not None and time_value <= previous_insert_time:
                time_value = previous_insert_time + minimum_duration

            next_time = write_steps[index + 1][0] if index + 1 < len(write_steps) and write_steps[index + 1][0] > time_value else None
            if next_time is not None:
                duration = max(minimum_duration, next_time - time_value)
            else:
                duration = max(minimum_duration, info["physical_end"] - time_value) if info["physical_end"] > time_value else max(minimum_duration, duration)

            try:
                envelope.insert_step(time_value, duration, raw_value)
                previous_insert_time = time_value
            except Exception:
                pass

    def _rewrite_all_decoupled_automation_envelopes(self, clip, info):
        if clip is None or not info:
            return

        sample_duration = 1.0 / 128.0
        for control_index in range(8):
            try:
                device_param = self._current_connected_parameter_for_control(control_index)
                self._rewrite_decoupled_automation_for_parameter(clip, device_param, control_index, info, sample_duration)
            except Exception as e:
                self._debug_log("Error rewriting decoupled automation for control {}: {}".format(control_index, str(e)))

    def _automation_steps_from_step_source(self, steps, start, length, sample_duration):
        steps = self._automation_sorted_steps(steps or ())
        if not steps:
            return tuple()

        start = max(0.0, float(start))
        length = max(0.0001, float(length))
        count = max(2, min(self.AUTOMATION_ENVELOPE_MAX_SAMPLES, int(math.ceil(length / sample_duration)) + 1))
        if count > 1:
            sample_duration = max(0.0001, length / float(count - 1))

        samples = []
        for index in range(count):
            time_value = start + (float(index) * sample_duration)
            samples.append((time_value, self._automation_value_from_steps(time_value, steps)))

        return self._automation_sorted_steps(
            (time_value, sample_duration, normalized, 0.0, 0, 0)
            for time_value, normalized in self._compress_automation_samples(samples)
        )

    def _couple_decoupled_automation_for_parameter(self, clip, device_param, control_index, info, target_length, sample_duration):
        if clip is None or device_param is None or not liveobj_valid(device_param) or not info:
            return

        parameter_key = self._decoupled_automation_parameter_key(device_param)
        if not parameter_key:
            return

        source_info = self._decoupled_info_for_parameter_key(info, parameter_key)
        target_length = max(0.0001, float(target_length))
        cleanup_length = max(float(info.get("physical_length", target_length)), target_length)
        source_info["physical_length"] = cleanup_length
        source_info["physical_end"] = source_info["note_start"] + cleanup_length

        envelope = None
        if hasattr(clip, "automation_envelope"):
            try:
                envelope = clip.automation_envelope(device_param)
            except Exception:
                envelope = None

        authored_steps = self._authored_automation_steps(clip, device_param, control_index)
        if authored_steps is not None:
            logical_steps = self._normalize_decoupled_logical_automation_steps(source_info, authored_steps)
        elif envelope is not None:
            logical_steps = self._automation_steps_from_envelope_samples(
                envelope,
                device_param,
                source_info["note_start"],
                source_info["automation_length"],
                sample_duration
            )
        else:
            return

        if not logical_steps:
            return

        expanded_steps = self._expanded_decoupled_automation_steps(source_info, logical_steps, sample_duration)
        coupled_steps = self._automation_steps_from_step_source(
            expanded_steps,
            source_info["note_start"],
            target_length,
            sample_duration
        )
        if not coupled_steps:
            return

        if envelope is None and hasattr(clip, "create_automation_envelope"):
            try:
                envelope = clip.create_automation_envelope(device_param)
            except Exception:
                envelope = None
        if envelope is None:
            return

        coupled_info = {
            "note_start": source_info["note_start"],
            "note_length": target_length,
            "note_end": source_info["note_start"] + target_length,
            "automation_lengths": {},
            "automation_length": target_length,
            "physical_length": target_length,
            "physical_end": source_info["note_start"] + target_length,
        }
        write_steps = self._sampled_decoupled_automation_write_steps(coupled_info, coupled_steps, sample_duration)
        if not write_steps:
            return

        self._neutralize_decoupled_automation_points(
            envelope,
            device_param,
            source_info,
            logical_steps,
            write_steps[0][2],
            sample_duration
        )
        self._write_automation_steps_to_envelope(envelope, device_param, coupled_info, write_steps)
        self._store_authored_automation_steps(clip, device_param, control_index, coupled_steps)

    def _couple_decoupled_automation_to_loop_length(self, clip, target_length):
        info = self._decoupled_automation_info(clip)
        if not info:
            return False

        sample_duration = 1.0 / 128.0
        for control_index in range(8):
            try:
                device_param = self._current_connected_parameter_for_control(control_index)
                self._couple_decoupled_automation_for_parameter(clip, device_param, control_index, info, target_length, sample_duration)
            except Exception as e:
                self._debug_log("Error coupling decoupled automation for control {}: {}".format(control_index, str(e)))

        self._remove_decoupled_automation_info_from_name(clip)
        return True

    def _duplicate_loop_automation_to_loop_length(self, clip, source_start, source_length, target_length):
        if clip is None:
            return False

        source_start = max(0.0, float(source_start))
        source_length = max(0.0001, float(source_length))
        target_length = max(source_length, float(target_length))
        sample_duration = 1.0 / 128.0
        duplicated_any = False

        for control_index in range(8):
            try:
                device_param = self._current_connected_parameter_for_control(control_index)
                if self._duplicate_loop_automation_for_parameter(
                    clip,
                    device_param,
                    control_index,
                    source_start,
                    source_length,
                    target_length,
                    sample_duration
                ):
                    duplicated_any = True
            except Exception as e:
                self._debug_log("Error duplicating loop automation for control {}: {}".format(control_index, str(e)))

        return duplicated_any

    def _duplicate_loop_automation_for_parameter(self, clip, device_param, control_index, source_start, source_length, target_length, sample_duration):
        if clip is None or device_param is None or not liveobj_valid(device_param):
            return False

        envelope = None
        if hasattr(clip, "automation_envelope"):
            try:
                envelope = clip.automation_envelope(device_param)
            except Exception:
                envelope = None

        authored_steps = self._authored_automation_steps(clip, device_param, control_index)
        if authored_steps is not None:
            logical_steps = self._automation_steps_from_step_source(
                authored_steps,
                source_start,
                source_length,
                sample_duration
            )
        elif envelope is not None:
            logical_steps = self._automation_steps_from_envelope_samples(
                envelope,
                device_param,
                source_start,
                source_length,
                sample_duration
            )
        else:
            return False

        if not logical_steps:
            return False

        expanded_source_length = source_length * max(1, int(math.ceil(target_length / source_length)))
        loop_info = {
            "note_start": source_start,
            "note_length": source_length,
            "note_end": source_start + source_length,
            "automation_lengths": {},
            "automation_length": source_length,
            "physical_length": expanded_source_length,
            "physical_end": source_start + expanded_source_length,
        }
        logical_steps = self._normalize_decoupled_logical_automation_steps(loop_info, logical_steps)
        if not logical_steps:
            return False

        expanded_steps = self._expanded_decoupled_automation_steps(loop_info, logical_steps, sample_duration)
        duplicated_steps = self._automation_steps_from_step_source(
            expanded_steps,
            source_start,
            target_length,
            sample_duration
        )
        if not duplicated_steps:
            return False

        coupled_info = {
            "note_start": source_start,
            "note_length": target_length,
            "note_end": source_start + target_length,
            "automation_lengths": {},
            "automation_length": target_length,
            "physical_length": target_length,
            "physical_end": source_start + target_length,
        }
        write_steps = self._sampled_decoupled_automation_write_steps(coupled_info, duplicated_steps, sample_duration)
        if not write_steps:
            return False

        if envelope is None and hasattr(clip, "create_automation_envelope"):
            try:
                envelope = clip.create_automation_envelope(device_param)
            except Exception:
                envelope = None
        if envelope is None:
            return False

        automation_should_re_enable = self._parameter_automation_is_enabled(device_param) or envelope is not None
        self._neutralize_decoupled_automation_points(
            envelope,
            device_param,
            coupled_info,
            duplicated_steps,
            write_steps[0][2],
            sample_duration
        )
        self._write_automation_steps_to_envelope(envelope, device_param, coupled_info, write_steps)
        self._store_authored_automation_steps(clip, device_param, control_index, duplicated_steps)
        self._re_enable_after_automation_write(device_param, automation_should_re_enable)
        return True

    def _merge_decoupled_automation_span(self, previous_steps, edited_steps, page_start, page_end):
        edited_steps = tuple(edited_steps or ())
        if previous_steps is None:
            return self._automation_sorted_steps(edited_steps)

        epsilon = 0.000001
        outside_steps = [
            step for step in previous_steps
            if step[0] < page_start - epsilon or step[0] > page_end + epsilon
        ]
        return self._automation_sorted_steps(outside_steps + list(edited_steps))

    def _normalize_decoupled_logical_automation_steps(self, info, steps):
        if not info:
            return tuple(steps or ())

        loop_start = info["note_start"]
        loop_end = info["note_start"] + info["automation_length"]
        epsilon = 0.000001
        normalized_steps = []
        for index, step in enumerate(tuple(steps or ())):
            time_value, duration, normalized, curve, step_id, step_order = self._automation_step_tuple(step, index)
            if time_value < loop_start - epsilon or time_value > loop_end + epsilon:
                continue
            folded_time = loop_end if time_value >= loop_end - epsilon else max(loop_start, time_value)
            normalized_steps.append((folded_time, duration, normalized, curve, step_id, step_order))

        return self._automation_sorted_steps(normalized_steps)

    def _follow_action_name_payload(self, rule):
        action_a, action_b = rule.get("actions", ({}, {}))
        return "|".join([
            str(max(1, int(rule.get("play_count", 1)))),
            str(max(0, min(100, int(rule.get("chance_a", 100))))),
            self._normalize_follow_action(action_a.get("type", "")),
            "" if action_a.get("jump_index") is None else str(int(action_a.get("jump_index"))),
            self._normalize_follow_action(action_b.get("type", "")),
            "" if action_b.get("jump_index") is None else str(int(action_b.get("jump_index"))),
        ])

    def _follow_action_marker_for_rule(self, rule):
        return "[TapFA:v1|{}]".format(self._follow_action_name_payload(rule))

    def _decode_follow_action_name_payload(self, payload, target_kind, track_index, scene_index):
        fields = str(payload or "").split("|")
        if len(fields) != 6:
            return None

        try:
            play_count = max(1, int(fields[0]))
            chance_a = max(0, min(100, int(fields[1])))
            action_a = self._normalize_follow_action(fields[2])
            jump_a = int(fields[3]) if fields[3] != "" else None
            action_b = self._normalize_follow_action(fields[4])
            jump_b = int(fields[5]) if fields[5] != "" else None
        except Exception:
            return None

        return {
            "target_kind": target_kind,
            "track_index": track_index,
            "scene_index": scene_index,
            "play_count": play_count,
            "chance_a": chance_a,
            "actions": (
                {"type": action_a, "jump_index": jump_a},
                {"type": action_b, "jump_index": jump_b},
            ),
        }

    def _follow_action_rule_from_name(self, name, target_kind, track_index, scene_index):
        matches = self.FOLLOW_ACTION_NAME_MARKER_RE.findall(str(name or ""))
        if not matches:
            return None
        return self._decode_follow_action_name_payload(matches[-1], target_kind, track_index, scene_index)

    def _follow_action_name_target(self, target_kind, track_index, scene_index):
        try:
            if target_kind == "clip":
                clip_slot = self.song().tracks[track_index].clip_slots[scene_index]
                if clip_slot.has_clip:
                    return clip_slot.clip
                return None
            if target_kind == "scene":
                return self.song().scenes[scene_index]
        except Exception:
            return None
        return None

    def _save_follow_action_rule_to_name(self, rule):
        target = self._follow_action_name_target(
            rule.get("target_kind"),
            rule.get("track_index"),
            rule.get("scene_index")
        )
        if target is None or not hasattr(target, "name"):
            return

        try:
            clean_name = self._strip_follow_action_name_marker(target.name)
            marker = self._follow_action_marker_for_rule(rule)
            new_name = "{} {}".format(clean_name, marker).strip() if clean_name else marker
            if target.name != new_name:
                target.name = new_name
        except Exception:
            pass

    def _remove_follow_action_rule_from_name(self, target_kind, track_index, scene_index):
        target = self._follow_action_name_target(target_kind, track_index, scene_index)
        if target is None or not hasattr(target, "name"):
            return

        try:
            clean_name = self._strip_follow_action_name_marker(target.name)
            if target.name != clean_name:
                target.name = clean_name
        except Exception:
            pass

    def _load_follow_actions_from_names(self, force_send=False):
        loaded_rules = {}

        try:
            for scene_index, scene in enumerate(self.song().scenes):
                rule = self._follow_action_rule_from_name(scene.name, "scene", None, scene_index)
                if rule:
                    loaded_rules[self._follow_action_key("scene", None, scene_index)] = rule

            for track_index, track in enumerate(self.song().tracks):
                for scene_index, clip_slot in enumerate(track.clip_slots):
                    if not clip_slot.has_clip:
                        continue
                    rule = self._follow_action_rule_from_name(clip_slot.clip.name, "clip", track_index, scene_index)
                    if rule:
                        loaded_rules[self._follow_action_key("clip", track_index, scene_index)] = rule
        except Exception:
            return

        if force_send or loaded_rules != self._follow_action_rules:
            self._follow_action_rules = loaded_rules
            self._active_follow_actions = {}
            self._handled_follow_action_launches = set()
            self._follow_action_missing_clip_counts = {}
            self._last_follow_action_state = None
            self._send_follow_action_state(force=True)

    def _decode_follow_action_rule(self, message):
        try:
            raw_payload = bytes(self._follow_action_payload_bytes(message)).decode("ascii")
        except Exception:
            return None

        fields = self._split_escaped_sysex_fields(raw_payload, "|")
        if len(fields) < 9:
            return None

        try:
            target_kind = self._unescape_sysex_string(fields[0])
            track_index = int(fields[1]) if fields[1] != "" else None
            scene_index = int(fields[2])
            play_count = max(1, int(fields[3]))
            chance_a = max(0, min(100, int(fields[4])))
            action_a = self._normalize_follow_action(fields[5])
            jump_a = int(fields[6]) if fields[6] != "" else None
            action_b = self._normalize_follow_action(fields[7])
            jump_b = int(fields[8]) if fields[8] != "" else None
        except Exception as error:
            return None

        if target_kind not in ("clip", "scene"):
            return None

        return {
            "target_kind": target_kind,
            "track_index": track_index,
            "scene_index": scene_index,
            "play_count": play_count,
            "chance_a": chance_a,
            "actions": (
                {"type": action_a, "jump_index": jump_a},
                {"type": action_b, "jump_index": jump_b},
            ),
        }

    def _set_follow_action_rule(self, message):
        rule = self._decode_follow_action_rule(message)
        if not rule:
            return
        key = self._follow_action_key(rule["target_kind"], rule.get("track_index"), rule["scene_index"])
        self._follow_action_rules[key] = rule
        self._save_follow_action_rule_to_name(rule)
        self._sync_follow_action_name_listeners()
        self._send_follow_action_state()

    def _delete_follow_action_rule(self, message):
        try:
            raw_payload = bytes(self._follow_action_payload_bytes(message)).decode("ascii")
            fields = self._split_escaped_sysex_fields(raw_payload, "|")
            target_kind = self._unescape_sysex_string(fields[0])
            track_index = int(fields[1]) if fields[1] != "" else None
            scene_index = int(fields[2])
            key = self._follow_action_key(target_kind, track_index, scene_index)
            if key in self._follow_action_rules:
                del self._follow_action_rules[key]
            self._remove_follow_action_rule_from_name(target_kind, track_index, scene_index)
            if key in self._active_follow_actions:
                del self._active_follow_actions[key]
            self._handled_follow_action_launches.discard(key)
            self._follow_action_missing_clip_counts.pop(key, None)
            self._sync_follow_action_name_listeners()
        except Exception:
            pass
        self._send_follow_action_state()

    def _activate_follow_action_for_clip(self, track_index, scene_index, clip_slot):
        if not clip_slot or not clip_slot.has_clip:
            return
        key = self._follow_action_key("clip", track_index, scene_index)
        if key in self._handled_follow_action_launches:
            return
        rule = self._follow_action_rules.get(key)
        if not rule:
            return

        self._active_follow_actions[key] = {
            "key": key,
            "rule": rule,
            "target_kind": "clip",
            "track_index": track_index,
            "scene_index": scene_index,
            "clip_slot": clip_slot,
            "started_at": None,
            "waiting_for_launch": (not clip_slot.is_playing) or self._clip_slot_is_triggered(clip_slot),
            "executed": False,
        }
        self._send_follow_action_state()

    def _activate_follow_action_for_scene(self, scene_index):
        key = self._follow_action_key("scene", None, scene_index)
        if key in self._handled_follow_action_launches:
            return
        rule = self._follow_action_rules.get(key)
        if not rule:
            self._send_follow_action_state()
            return

        self._active_follow_actions[key] = {
            "key": key,
            "rule": rule,
            "target_kind": "scene",
            "track_index": None,
            "scene_index": scene_index,
            "started_at": None,
            "waiting_for_launch": True,
            "executed": False,
        }
        self._send_follow_action_state()

    def _evaluate_follow_actions(self):
        self._clear_finished_follow_action_launches()
        if not self._active_follow_actions:
            return

        try:
            song_time = float(self.song().current_song_time)
        except Exception:
            return

        changed = False
        active_items = sorted(
            list(self._active_follow_actions.items()),
            key=lambda item: 1 if item[1].get("target_kind") == "scene" else 0
        )

        for key, active in active_items:
            if key not in self._active_follow_actions or active.get("executed"):
                continue

            target_kind = active.get("target_kind")
            scene_index = active.get("scene_index")
            clip_slot = active.get("clip_slot")

            if target_kind == "clip":
                if not clip_slot or not clip_slot.has_clip:
                    del self._active_follow_actions[key]
                    changed = True
                    continue
                if not clip_slot.is_playing:
                    if self._clip_slot_is_triggered(clip_slot):
                        continue
                    del self._active_follow_actions[key]
                    changed = True
                    continue
                if active.get("waiting_for_launch"):
                    if self._clip_slot_is_triggered(clip_slot):
                        continue
                    active["waiting_for_launch"] = False
                    active["started_at"] = song_time
                    changed = True
                    continue
                base_length = self._clip_slot_length_beats(clip_slot)
            else:
                if not self._scene_is_playing(scene_index):
                    if active.get("waiting_for_launch") and self._scene_is_triggered(scene_index):
                        continue
                    del self._active_follow_actions[key]
                    changed = True
                    continue
                if active.get("waiting_for_launch"):
                    if self._scene_is_triggered(scene_index):
                        continue
                    active["waiting_for_launch"] = False
                    active["started_at"] = song_time
                    changed = True
                    continue
                base_length = self._scene_length_beats(scene_index)

            if base_length <= 0.0:
                continue

            if active.get("started_at") is None:
                active["started_at"] = song_time

            rule = active.get("rule", {})
            total_beats = base_length * max(1, int(rule.get("play_count", 1)))
            launch_quantization_beats = self._get_global_launch_quantization_beats()
            trigger_after = max(0.0, total_beats - launch_quantization_beats)

            if song_time - active["started_at"] >= trigger_after:
                active["executed"] = True
                del self._active_follow_actions[key]
                launched_key = self._execute_follow_action(active)
                if launched_key != key:
                    self._handled_follow_action_launches.add(key)
                changed = True

        if changed:
            self._send_follow_action_state()

    def _choose_follow_action(self, rule):
        chance_a = max(0, min(100, int(rule.get("chance_a", 100))))
        return rule.get("actions", ({}, {}))[0 if random.randint(1, 100) <= chance_a else 1]

    def _execute_follow_action(self, active):
        rule = active.get("rule", {})
        action = self._choose_follow_action(rule)
        action_type = action.get("type")
        if action_type in (None, "", "none"):
            return None

        if active.get("target_kind") == "scene":
            return self._execute_scene_follow_action(active.get("scene_index"), action)
        else:
            return self._execute_clip_follow_action(active.get("track_index"), active.get("scene_index"), action)

    def _valid_scene_indexes(self):
        return [index for index, _ in enumerate(self.song().scenes) if self._scene_has_clip(index)]

    def _valid_clip_indexes(self, track_index):
        try:
            return [index for index, clip_slot in enumerate(self.song().tracks[track_index].clip_slots) if clip_slot.has_clip]
        except Exception:
            return []

    def _pick_index_for_action(self, indexes, current_index, action):
        action_type = action.get("type")
        if not indexes:
            return None
        if action_type == "first":
            return indexes[0]
        if action_type == "last":
            return indexes[-1]
        if action_type == "previous":
            previous = [index for index in indexes if index < current_index]
            return previous[-1] if previous else indexes[-1]
        if action_type == "next":
            next_indexes = [index for index in indexes if index > current_index]
            return next_indexes[0] if next_indexes else indexes[0]
        if action_type == "any":
            return random.choice(indexes)
        if action_type == "other":
            other_indexes = [index for index in indexes if index != current_index]
            return random.choice(other_indexes or indexes)
        if action_type == "jump":
            jump_index = action.get("jump_index")
            return jump_index if jump_index in indexes else None
        if action_type == "play_again":
            return current_index
        return None

    def _execute_scene_follow_action(self, scene_index, action):
        action_type = action.get("type")
        if action_type == "stop":
            self.song().stop_all_clips()
            return
        if action_type == "jump":
            target_index = action.get("jump_index")
        else:
            target_index = self._pick_index_for_action(self._valid_scene_indexes(), scene_index, action)
        if target_index is not None and target_index < len(self.song().scenes):
            self.song().scenes[target_index].fire()
            target_key = self._follow_action_key("scene", None, target_index)
            self._handled_follow_action_launches.discard(target_key)
            self._activate_follow_action_for_scene(target_index)
            return target_key
        return None

    def _execute_clip_follow_action(self, track_index, scene_index, action):
        try:
            track = self.song().tracks[track_index]
            clip_slot = track.clip_slots[scene_index]
        except Exception:
            return

        action_type = action.get("type")
        if action_type == "stop":
            clip_slot.stop()
            return None

        if action_type == "jump":
            target_index = action.get("jump_index")
        else:
            target_index = self._pick_index_for_action(self._valid_clip_indexes(track_index), scene_index, action)
        if target_index is not None and 0 <= target_index < len(track.clip_slots):
            target_slot = track.clip_slots[target_index]
            target_slot.fire()
            target_key = self._follow_action_key("clip", track_index, target_index)
            self._handled_follow_action_launches.discard(target_key)
            self._activate_follow_action_for_clip(track_index, target_index, target_slot)
            return target_key
        return None

    def _encode_follow_action_rule(self, rule):
        action_a, action_b = rule.get("actions", ({}, {}))
        return "|".join([
            "rule",
            rule.get("target_kind", ""),
            "" if rule.get("track_index") is None else str(rule.get("track_index")),
            str(rule.get("scene_index", 0)),
            str(rule.get("play_count", 1)),
            str(rule.get("chance_a", 100)),
            action_a.get("type", ""),
            "" if action_a.get("jump_index") is None else str(action_a.get("jump_index")),
            action_b.get("type", ""),
            "" if action_b.get("jump_index") is None else str(action_b.get("jump_index")),
        ])

    def _send_follow_action_state(self, force=False):
        rules = [self._encode_follow_action_rule(rule) for rule in self._follow_action_rules.values()]
        for active in self._active_follow_actions.values():
            rule = active.get("rule", {})
            rules.append("|".join([
                "active",
                active.get("target_kind", ""),
                "" if active.get("track_index") is None else str(active.get("track_index")),
                str(active.get("scene_index", 0)),
                "1",
            ]))
        payload = ";".join(rules)
        if force or payload != self._last_follow_action_state:
            self._last_follow_action_state = payload
            if not self.was_initialized:
                return
            self._send_sys_ex_message(payload, 0x18)

    def _reconcile_follow_action_rules(self, force=False, remove_missing_clips=True):
        changed = False
        reconciled_rules = {}
        seen_keys = set()

        for key, rule in self._follow_action_rules.items():
            target_kind = rule.get("target_kind")
            scene_index = rule.get("scene_index")

            if target_kind == "clip":
                track_index = rule.get("track_index")
                try:
                    clip_slot = self.song().tracks[track_index].clip_slots[scene_index]
                    if not clip_slot.has_clip:
                        if remove_missing_clips:
                            missing_count = self._follow_action_missing_clip_counts.get(key, 0) + 1
                            self._follow_action_missing_clip_counts[key] = missing_count
                            if missing_count >= 4:
                                changed = True
                                self._follow_action_missing_clip_counts.pop(key, None)
                                continue
                        else:
                            self._follow_action_missing_clip_counts.pop(key, None)
                    else:
                        self._follow_action_missing_clip_counts.pop(key, None)
                except Exception:
                    changed = True
                    self._follow_action_missing_clip_counts.pop(key, None)
                    continue

                key = self._follow_action_key("clip", track_index, scene_index)
                seen_keys.add(key)
                reconciled_rules[key] = rule

            elif target_kind == "scene":
                try:
                    if scene_index is None or scene_index >= len(self.song().scenes):
                        changed = True
                        continue
                except Exception:
                    changed = True
                    continue

                key = self._follow_action_key("scene", None, scene_index)
                seen_keys.add(key)
                reconciled_rules[key] = rule

        for missing_key in list(self._follow_action_missing_clip_counts.keys()):
            if missing_key not in seen_keys:
                self._follow_action_missing_clip_counts.pop(missing_key, None)

        if force or changed:
            self._follow_action_rules = reconciled_rules
            self._active_follow_actions = {
                key: active for key, active in self._active_follow_actions.items()
                if key in self._follow_action_rules
            }
            self._handled_follow_action_launches = {
                key for key in self._handled_follow_action_launches
                if key in self._follow_action_rules
            }
            self._send_follow_action_state()

    def _redo_button_value(self, value):
        if value != 0:
            song = self.song()
            if song.can_redo:
                song.redo()
                # self._periodic_check()

    def _undo_button_value(self, value):
        if value != 0:
            song = self.song()
            if song.can_undo:
                song.undo()
                # self._periodic_check()

    def _sesh_record_value(self, value):
        if value != 0:
            record = self.song().session_record
            if record == False:
                self.song().session_record = True
            else:
                self.song().session_record = False
            self._send_session_record_state(force=True)
            self._set_up_notes_playing("clip")

    def _capture_button_value(self, value):
        if value != 0:
            self.song().capture_midi()
            self._set_up_notes_playing("clip")

    def _quantize_grid_value(self, value):
        global quantize_grid_value
        quantize_grid_value = value

    def _quantize_strength_value(self, value):
        global quantize_strength_value
        quantize_strength_value = value / 100.0

    def _swing_amount_value(self, value):
        global swing_amount_value
        # 100% swing amount did strange things, so I went down to 10% max
        swing_amount_value = value / 1000.0

    def _quantize_button_value(self, value):
        if value != 0:
            clip = self.song().view.detail_clip
            if clip:
                # need to set the swing amount first (0.00-1.00)
                self.song().swing_amount = swing_amount_value
                # grid (int 1 == 1/4, 2 == 1/8, 5 == 1/16, 8 = 1/32), strength (0.50 == 50%)
                clip.quantize(quantize_grid_value, quantize_strength_value)

    def _duplicate_button_value(self, value):
        if value != 0:
            self._duplicate_clip()

    def _duplicate_scene_button_value(self, value):
        if value != 0:
            song = self.song()
            selected_scene = song.view.selected_scene
            all_scenes = song.scenes
            current_index = list(all_scenes).index(selected_scene)
            song.duplicate_scene(current_index)
            destination_index = current_index + 1
            self._shift_follow_actions_after_scene_insert(destination_index)
            self._duplicate_follow_actions_for_scene(current_index, destination_index)
            self._send_follow_action_state()

    def _add_empty_clip(self, value):
        """
        Adds an empty clip at the currently highlighted track and scene.
        The clip length matches the time signature numerator (one full bar).
        """
        if value != 0:
            song = self.song()
            selected_track = song.view.selected_track
    
            if selected_track is None:
                return
            
            selected_scene = song.view.selected_scene
            all_scenes = song.scenes
            scene_index = list(all_scenes).index(selected_scene)
        
            clip_slot = selected_track.clip_slots[scene_index]
            
            if clip_slot.has_clip:
                return  # Avoid overwriting an existing clip
            
            clip_length = song.signature_numerator  # Use the time signature numerator for one full bar
            clip_slot.create_clip(clip_length)
            
            # lil trick to get the clip metadata to fire
            if self.seq_status:
                if self.device_status:
                    self.start_step_seq()
    
    def _create_new_empty_clip(self, value):
        """
        Creates an empty clip at the next empty the slot in the currently highlighted track.
        The clip length matches the time signature numerator (one full bar).
        """
        if value != 0:
            song = self.song()
            selected_track = song.view.selected_track
    
            if selected_track is None:
                return
            
            selected_scene = song.view.selected_scene
            all_scenes = song.scenes
            scene_index = list(all_scenes).index(selected_scene)
            destination_scene_index = len(all_scenes)
    
            # check if there is a free clip slot after the current clip
            for index, clip_slot in enumerate(selected_track.clip_slots):
                if index <= scene_index:
                    continue
                if clip_slot.has_clip:
                    continue
                destination_scene_index = index
                break
    
            if destination_scene_index == len(all_scenes):
                # create a new scene if there is no free slot after the current slot
                song.create_scene(-1)
        
            # adding empty clip at next empty scene
            clip_slot = selected_track.clip_slots[destination_scene_index]
            clip_length = song.signature_numerator  # Use the time signature numerator for one full bar
            clip_slot.create_clip(clip_length)
            
            # selecting the empty scene
            song.view.selected_scene = song.scenes[destination_scene_index]
    
    def _select_playing_clip(self, value):
        if value != 0:
            song = self.song()
            selected_track = song.view.selected_track
    
            if selected_track is None:
                return
            
            for index, clip_slot in enumerate(selected_track.clip_slots):
                if clip_slot.is_playing:
                    song.view.selected_scene = song.scenes[index]
                    break
    
    def _crop_clip(self, value):
        if value != 0:
            song = self.song()
            clip_slot = song.view.highlighted_clip_slot
            
            if clip_slot and clip_slot.has_clip:
                clip = clip_slot.clip
                clip.crop()
    
    def _duplicate_clip(self):
        song = self.song()
        selected_track = song.view.selected_track

        if selected_track is None:
            return

        selected_scene = song.view.selected_scene
        all_scenes = song.scenes
        scene_index = list(all_scenes).index(selected_scene)
        track_index = list(song.tracks).index(selected_track)
        destination_scene_index = len(all_scenes)

        # checking if clip was playing
        was_playing = song.view.highlighted_clip_slot.is_playing == 1

        # check if there is a free clip slot after the current clip
        for index, clip_slot in enumerate(selected_track.clip_slots):
            if index <= scene_index:
                continue
            if clip_slot.has_clip:
                continue
            destination_scene_index = index
            break

        if destination_scene_index == len(all_scenes):
            # create a new scene if there is no free slot after the current slot
            song.create_scene(-1)

        self._copy_paste_clip(track_index, scene_index, track_index, destination_scene_index)

        # select newly created clip
        song.view.selected_scene = song.scenes[destination_scene_index]
        # fire the new clip if the old clip was playing
        if was_playing:
            song.view.highlighted_clip_slot.fire(force_legato=True)
        
        # set up everything new
        self._on_selected_track_changed()

    @subject_slot('selected_track')
    def _on_selected_track_changed(self):
        if self.was_initialized:
            selected_track = self.song().view.selected_track
            track_control_selected = self._track_device_is_selected(selected_track)
            self._track_device_selected = track_control_selected
            track_has_midi_input = 0
            if selected_track and selected_track.has_midi_input:
                self._set_selected_track_implicit_arm()
                track_has_midi_input = 1
            self._set_up_notes_playing(selected_track)
            self._set_other_tracks_implicit_arm()
            # send new index of selected track
            self._send_selected_track_index(selected_track)
            self._on_selected_scene_changed()
            # send sys ex of track midi input status.
            self._send_sys_ex_message(str(track_has_midi_input), 0x0B)
            # TODO: this part doesn't seem to work? how can I make this work with master and return?
            device_to_select = selected_track.view.selected_device
            if device_to_select is None and len(selected_track.devices) > 0:
                device_to_select = selected_track.devices[0]
            if track_control_selected:
                self._last_automation_signature = None
                self._on_device_changed()
            elif device_to_select is not None:
                self._track_change_in_progress = True
                self.song().view.select_device(device_to_select)
                self._track_change_in_progress = False
                self._device_component.set_device(device_to_select)
                self._last_automation_signature = None
                self._on_device_changed()
            else:
                self._device_component.set_device(device_to_select)
                self._last_automation_signature = None
                self._on_device_changed()
            self._check_clip_playing_status(force=True)
            if self.seq_status:
                if self.device_status:
                    self.start_step_seq()

    def _set_up_notes_playing(self, selected_track):
        if selected_track != "clip":
            # Only remove playing position listeners from the PREVIOUSLY selected track,
            # not from all tracks. This changes O(tracks * clips) to O(clips).
            if hasattr(self, '_previous_selected_track') and self._previous_selected_track is not None:
                old_track = self._previous_selected_track
                if liveobj_valid(old_track):
                    for (clip_index, clip_slot) in enumerate(old_track.clip_slots):
                        if clip_slot is not None and clip_slot.has_clip:
                            if clip_slot.clip.playing_position_has_listener(self.playing_position_listeners[clip_index]):
                                clip_slot.clip.remove_playing_position_listener(self.playing_position_listeners[clip_index])
            self._previous_selected_track = selected_track
        else:
            selected_track = self.song().view.selected_track

        if selected_track.has_midi_input:
            for (clip_index, clip_slot) in enumerate(selected_track.clip_slots):
                if clip_slot is not None and clip_slot.has_clip:
                    if not clip_slot.clip.playing_position_has_listener(self.playing_position_listeners[clip_index]):
                        # self.log_message("adding pos listener: {}".format(clip_index))
                        listener = lambda index=clip_index: self._clip_pos_changed(index)
                        self.playing_position_listeners[clip_index] = listener
                        clip_slot.clip.add_playing_position_listener(listener)

    def _check_clip_playing_status(self, force=False):
        try:
            song = self.song()
            selected_track = song.view.selected_track
            highlighted_clip_slot_playing = getattr(song.view.highlighted_clip_slot, 'is_playing', False)
            
            if highlighted_clip_slot_playing:
                # Ensure status is 0 if the highlighted clip is playing
                new_status = 0
            else:
                # Check if any other clip is playing
                another_clip_playing = any(clip_slot.is_playing for clip_slot in selected_track.clip_slots if clip_slot.has_clip)
                new_status = 1 if another_clip_playing else 2
        except Exception:
            return
        
        # Update status only if it has changed
        if force or self.seq_clip_playing_status != new_status:
            self.seq_clip_playing_status = new_status
            self.send_cc(67, 11, new_status)

    def _clip_pos_changed(self, clip_index):
        # Only check and send things if we are in device view
        if self.device_status:
            song = self.song()
            selected_track = song.view.selected_track

            if clip_index < len(selected_track.clip_slots):
                clip_slot = selected_track.clip_slots[clip_index]

                if clip_slot is not None and clip_slot.has_clip:
                    clip_playing = clip_slot.clip
                    
                    clip_start = min(clip_playing.start_time, clip_playing.start_marker, clip_playing.loop_start) - self.clip_length_trick
                    time_span = (max(clip_playing.loop_end, clip_playing.end_marker, clip_playing.length) + self.clip_length_trick) - clip_start
                    loop_start = clip_playing.loop_start
                    
                    try:
                        # Get all the notes in the clip
                        current_raw_notes = clip_playing.get_notes_extended(0, 128, clip_start, time_span)
                        
                        # if the current clip has different notes save the new notes.
                        if current_raw_notes != self.last_raw_notes:
                            self.last_raw_notes = current_raw_notes

                            # Reset the current clip notes array
                            self.current_clip_notes = []
                            # add all the notes to the array
                            for midi_note in current_raw_notes:
                                pitch = midi_note.pitch
                                duration = midi_note.duration
                                note_start_time = midi_note.start_time
                                # Process note properties as needed
                                # self.log_message("Note: Pitch {}, Start Time {}, Duration {}".format(pitch, start_time, duration))
                                end_time = note_start_time + duration
                                self.current_clip_notes.append([pitch, note_start_time, end_time])

                        # check which notes are playing at position
                        # if we detect changes send them out to app
                        clip_position = clip_playing.playing_position
                        
                        # clip status no mater the seq_status
                        self._check_clip_playing_status()
                        
                        if self.seq_status:
                            if song.view.highlighted_clip_slot.is_playing:
                                self.send_out_playing_pos(clip_position, clip_playing.signature_numerator)
                                self.last_sent_out_playing_pos = clip_position
                            else:
                                # reseting the playing position
                                if self.last_sent_out_playing_pos != 0.0:
                                    self.last_sent_out_playing_pos = 0.0
                                    self.send_out_playing_pos(self.last_sent_out_playing_pos, 1.0, force=True, hidden=True)
                                
                        else:
                            # making sure we have the right starting position, when jumping back to the start of clip or loop
                            if self.last_playing_position > clip_position:
                                if clip_position >= loop_start:
                                    self.last_playing_position = loop_start
                                else:
                                    self.last_playing_position = clip_playing.start_marker
    
                            # check if currently playing notes are still playing in this playing position
                            for (note_index, is_playing) in enumerate(self.currently_playing_notes):
                                if is_playing:
                                    # Initialize a flag to indicate if a playing note was found
                                    found_playing_note = False
    
                                    # Find the notes that stopped playing in since the last update
                                    for note in self.current_clip_notes:
                                        pitch, note_start_time, end_time = note
    
                                        if note_start_time <= self.last_playing_position and clip_position < end_time and pitch == note_index:
                                            # Note is still playing
                                            found_playing_note = True
                                            break
    
                                    # If no playing note was found, update the state
                                    if not found_playing_note:
                                        self.currently_playing_notes[note_index] = False
                                        # send note off for note_index note.
                                        # self.log_message("Note off: {}".format(note_index))
                                        self.send_note_off(note_index, 0, 100)
    
                            # check current clip notes array which notes are on for that playing position
                            for note in self.current_clip_notes:
                                pitch, note_start_time, end_time = note
                                if self.last_playing_position <= note_start_time <= clip_position:
                                    # note starts playing
                                    self.currently_playing_notes[pitch] = True
                                    # send midi note on
                                    # self.log_message("Note on: {}".format(pitch))
                                    self.send_note_on(pitch, 0, 100)
                            # update last playing position
                            self.last_playing_position = clip_position
                    except Exception as e:
                        self._debug_log(f"Exception for clip position changed: {str(e)}")
                        import traceback
                        self._debug_log(traceback.format_exc())
                        pass
                # else:
                    # self.log_message("No valid clip in the slot.")

    def _send_selected_track_index(self, selected_track):
        track_list = self.song().tracks
        track_index = self._find_track_index(selected_track, track_list)
        self._send_sys_ex_message(track_index, 0x03)
        if track_index == "not found":
            return_tracks_list = self.song().return_tracks
            return_track_index = self._find_track_index(selected_track, return_tracks_list)
            if return_track_index == "not found":
                return_track_index = str(len(return_tracks_list))
            self._send_sys_ex_message(return_track_index, 0x08)
        else:
            self._send_sys_ex_message("none selected", 0x08)

    def _find_track_index(self, track, track_list):
        for index, t in enumerate(track_list):
            if track == t:
                return str(index)
        return "not found" # Track not found

    def _select_device_by_index(self, value):
        selected_track = self.song().view.selected_track
        was_track_control = self._track_device_is_selected(selected_track)
        if value == 0:
            self._set_track_device_selected(True, selected_track)
            self._connect_track_device_parameter_controls(selected_track)
            try:
                self.request_rebuild_midi_map()
            except Exception:
                pass
            self._on_device_changed()
            return

        self._set_track_device_selected(False, selected_track)
        if was_track_control:
            self._connect_device_controls()
            try:
                self.request_rebuild_midi_map()
            except Exception:
                pass
        all_devices = self._get_all_nested_devices(selected_track.devices)[0]
        live_index = self._app_device_index_to_live_index(value)
        if live_index < 0 or live_index >= len(all_devices):
            return
        device_to_select = all_devices[live_index]
        was_selected_device = device_to_select == selected_track.view.selected_device
        self.song().view.select_device(device_to_select)
        if was_selected_device:
            if hasattr(self, "_device") and hasattr(self._device, "set_device"):
                self._device.set_device(device_to_select)
            self._last_automation_signature = None
            self._on_device_changed()

    def _select_track_by_index(self, track_index):
        # self.log_message("Getting track: {}".format(track_index))
        song = self.song()
        if track_index >= 0 and track_index < len(song.tracks):
            song.view.selected_track = song.tracks[track_index]
        else:
            self._debug_log("Invalid track index: {}".format(track_index))

    def _select_return_track_by_index(self, track_index):
        song = self.song()
        if track_index < len(song.return_tracks):
            return_track = song.return_tracks[track_index] 
            song.view.selected_track = return_track
        else:
            master_track = song.master_track 
            song.view.selected_track = master_track

    def _set_selected_track_implicit_arm(self):
        selected_track = self.song().view.selected_track
        if selected_track and selected_track.has_midi_input:
            try:
                selected_track.implicit_arm = True
            except Exception:
                pass
        # else:
        #     try:
        #         self.song().tracks[0].implicit_arm = True
        #     except Exception:
        #         pass

    def _set_other_tracks_implicit_arm(self):
        for track in self.song().tracks:
            if track != self.song().view.selected_track:
                try:
                    track.implicit_arm = False
                except Exception:
                    pass

    def _on_tracks_changed(self):
        if self._metadata_recheck_timer:
            self._metadata_recheck_timer.cancel()
            self._metadata_recheck_timer = None
        self._metadata_cache.clear()
        self._metadata_send_seq_by_device.clear()
        self._sync_follow_actions_to_track_topology()
        self._update_mixer_and_tracks()
        self._register_clip_listeners()
        self._update_clip_slots()

    def _make_color_string(self, color):
        red = (color >> 16) & 255
        green = (color >> 8) & 255
        blue = color & 255
        color_string = "({},{},{})".format(red, green, blue)
        return color_string

    def _on_color_name_changed(self):
        self._update_mixer_and_tracks()
        self._on_selected_track_changed()
    
    def _create_level_change_handler(self, index):
        """
        A factory that returns a handler function for a specific index.
        This guarantees the index is correctly captured.
        """
        def handler():
            self._on_output_level_changed(index)
        return handler

    def _foldable_group_track_for_track(self, track):
        try:
            if track and liveobj_valid(track) and getattr(track, 'is_foldable', False):
                return track
        except Exception:
            pass

        try:
            if track and liveobj_valid(track) and getattr(track, 'is_grouped', False):
                group_track = getattr(track, 'group_track', None)
                if group_track and liveobj_valid(group_track) and getattr(group_track, 'is_foldable', False):
                    return group_track
        except Exception:
            pass

        return None

    def _group_fold_state_codes(self, tracks):
        fold_states = []
        for track in tracks:
            group_track = self._foldable_group_track_for_track(track)
            if group_track is None:
                fold_states.append("-")
                continue

            try:
                fold_states.append("1" if bool(group_track.fold_state) else "0")
            except Exception:
                fold_states.append("-")

        return tuple(fold_states)

    def _send_group_fold_states_if_changed(self, tracks=None, force=False):
        if tracks is None:
            tracks = list(self.song().tracks)

        fold_states = self._group_fold_state_codes(tracks)
        if force or fold_states != self._last_group_fold_states:
            self._last_group_fold_states = fold_states
            self._send_sys_ex_message(",".join(fold_states), 0x2B)

        hidden_states = self._group_hidden_state_codes(tracks)
        if force or hidden_states != self._last_group_hidden_states:
            self._last_group_hidden_states = hidden_states
            self._send_sys_ex_message(",".join(hidden_states), 0x2D)

    def _group_hidden_state_codes(self, tracks):
        hidden_states = []
        for track in tracks:
            hidden = False
            try:
                if track and liveobj_valid(track) and getattr(track, 'is_grouped', False):
                    group_track = getattr(track, 'group_track', None)
                    hidden = bool(group_track and liveobj_valid(group_track) and group_track.fold_state)
            except Exception:
                hidden = False

            hidden_states.append("1" if hidden else "0")

        return tuple(hidden_states)
    
    # Updating names and number of tracks
    def _format_track_name_for_display(self, name):
        name = str(name)
        dash_index = name.find("-")
        if dash_index <= 0:
            return name

        prefix = name[:dash_index]
        if not (prefix.isdigit() or (dash_index == 1 and prefix.isalpha())):
            return name

        return "{} {}".format(prefix, name[dash_index + 1:].lstrip())

    def _update_mixer_and_tracks(self):
        tracks = list(self.song().tracks)
        return_tracks = list(self.song().return_tracks)
        master_track = self.song().master_track

        track_signature = (
            tuple(
                (
                    id(track),
                    str(track.name),
                    int(track.color),
                    bool(track.is_grouped),
                    bool(track.has_audio_input),
                    any(clip_slot.is_group_slot for clip_slot in track.clip_slots),
                )
                for track in tracks
            ),
            tuple((id(track), str(track.name), int(track.color)) for track in return_tracks),
            int(master_track.color),
        )
        tracks_changed = track_signature != self._track_list_signature
        
        if tracks_changed:
            # 1. Remove all old listeners to prevent leaks
            for track, (left_listener, right_listener) in self._track_level_listeners.items():
                if liveobj_valid(track) and hasattr(track, 'output_meter_left_has_listener') and track.output_meter_left_has_listener(left_listener):
                    track.remove_output_meter_left_listener(left_listener)
                if liveobj_valid(track) and hasattr(track, 'output_meter_right_has_listener') and track.output_meter_right_has_listener(right_listener):
                    track.remove_output_meter_right_listener(right_listener)
            self._track_level_listeners.clear()

            for track, (left_listener, right_listener) in self._return_level_listeners.items():
                if liveobj_valid(track) and hasattr(track, 'output_meter_left_has_listener') and track.output_meter_left_has_listener(left_listener):
                    track.remove_output_meter_left_listener(left_listener)
                if liveobj_valid(track) and hasattr(track, 'output_meter_right_has_listener') and track.output_meter_right_has_listener(right_listener):
                    track.remove_output_meter_right_listener(right_listener)
            self._return_level_listeners.clear()
            
            if self._master_level_listeners:
                left_listener, right_listener = self._master_level_listeners.get(master_track, (None, None))
                if left_listener and hasattr(master_track, 'output_meter_left_has_listener') and master_track.output_meter_left_has_listener(left_listener):
                    master_track.remove_output_meter_left_listener(left_listener)
                if right_listener and hasattr(master_track, 'output_meter_right_has_listener') and master_track.output_meter_right_has_listener(right_listener):
                    master_track.remove_output_meter_right_listener(right_listener)
                self._master_level_listeners.clear()
            self._track_list_signature = track_signature

            # 2. Build track names, types, colors and send via SysEx
            track_names = []
            track_is_audio = []
            track_colors = []
            
            for index, track in enumerate(tracks):
                name = self._format_track_name_for_display(track.name)
                track_names.append(name)
                if any(clip_slot.is_group_slot for clip_slot in track.clip_slots):
                    track_is_audio.append("2")
                elif track.is_grouped:
                    if track.has_audio_input:
                        track_is_audio.append("4")
                    else:
                        track_is_audio.append("3")
                elif track.has_audio_input:
                    track_is_audio.append("1")
                else:
                    track_is_audio.append("0")
                color_string = self._make_color_string(track.color)
                track_colors.append(color_string)

            self._send_sys_ex_message(",".join(self._escape_sysex_string(name) for name in track_names), 0x02)
            self._send_sys_ex_message(",".join(track_is_audio), 0x0C)
            self._send_sys_ex_message("-".join(track_colors), 0x04)

            return_track_names = []
            return_track_colors = []
            for index, return_track in enumerate(return_tracks):
                name = self._format_track_name_for_display(return_track.name)
                return_track_names.append(name)
                return_track_colors.append(self._make_color_string(return_track.color))

            color_string = self._make_color_string(master_track.color)
            return_track_colors.append(color_string)
            self._send_sys_ex_message(",".join(self._escape_sysex_string(name) for name in return_track_names), 0x06)
            self._send_sys_ex_message("-".join(return_track_colors), 0x07)

        self._send_group_fold_states_if_changed(tracks)
        
        # 3. Set up level meter listeners (idempotent, safe to call every time)
        for index, track in enumerate(tracks):
            if track.has_audio_output:
                if track not in self._track_level_listeners:
                    left_handler = self._create_level_change_handler(index)
                    right_handler = self._create_level_change_handler(index)
                    track.add_output_meter_left_listener(left_handler)
                    track.add_output_meter_right_listener(right_handler)
                    self._track_level_listeners[track] = (left_handler, right_handler)

            if not track.color_has_listener(self._on_color_name_changed):
                track.add_color_listener(self._on_color_name_changed)
            if not track.name_has_listener(self._on_color_name_changed):
                track.add_name_listener(self._on_color_name_changed)

        for index, return_track in enumerate(return_tracks):
            if hasattr(return_track, 'add_output_meter_left_listener'):
                return_index = index + len(tracks)
                if return_track not in self._return_level_listeners:
                    left_handler = self._create_level_change_handler(return_index)
                    right_handler = self._create_level_change_handler(return_index)
                    return_track.add_output_meter_left_listener(left_handler)
                    return_track.add_output_meter_right_listener(right_handler)
                    self._return_level_listeners[return_track] = (left_handler, right_handler)

        if hasattr(master_track, 'add_output_meter_left_listener'):
            master_index = 127
            if master_track not in self._master_level_listeners:
                left_handler = self._create_level_change_handler(master_index)
                right_handler = self._create_level_change_handler(master_index)
                master_track.add_output_meter_left_listener(left_handler)
                master_track.add_output_meter_right_listener(right_handler)
                self._master_level_listeners[master_track] = (left_handler, right_handler)

        self._set_up_mixer_controls()

    def _create_mixer_automation_control(self, control_type, midi_channel, cc, channel_type, track_index, parameter_name, send_index=None):
        if control_type == "slider":
            control = SliderElement(MIDI_CC_TYPE, midi_channel, cc)
        else:
            control = EncoderElement(MIDI_CC_TYPE, midi_channel, cc, Live.MidiMap.MapMode.absolute)

        control.add_value_listener(
            lambda value, channel_type=channel_type, track_index=track_index, parameter_name=parameter_name, send_index=send_index:
                self._on_mixer_automation_control_value(value, channel_type, track_index, parameter_name, send_index)
        )
        parameter = self._mixer_parameter(channel_type, track_index, parameter_name, send_index)
        if parameter and liveobj_valid(parameter):
            control.connect_to(parameter)
        self._mixer_automation_controls.append(control)
        self._register_mixer_automation_status(midi_channel, cc, channel_type, track_index, parameter_name, send_index)
        return control

    def _create_mixer_toggle_control(self, midi_channel, cc, channel_type, track_index, property_name):
        control = ButtonElement(1, MIDI_CC_TYPE, midi_channel, cc)
        control.add_value_listener(
            lambda value, channel_type=channel_type, track_index=track_index, property_name=property_name:
                self._on_mixer_toggle_control_value(value, channel_type, track_index, property_name)
        )
        self._mixer_automation_controls.append(control)
        return control

    def _on_mixer_toggle_control_value(self, value, channel_type, track_index, property_name):
        if value == 0:
            return

        track = self._mixer_track(channel_type, track_index)
        if track is None:
            return

        try:
            setattr(track, property_name, not bool(getattr(track, property_name)))
        except Exception:
            pass

    def _release_mixer_controls(self):
        for control in list(getattr(self, '_mixer_automation_controls', [])):
            try:
                control.release_parameter()
            except Exception:
                pass
        self._mixer_automation_controls = []

    def _disconnect_mixer_component_controls(self):
        try:
            for index in range(127):
                strip = mixer.channel_strip(index)
                strip.set_volume_control(None)
                strip.set_send_controls(None)
                strip.set_pan_control(None)
                strip.set_mute_button(None)
                strip.set_solo_button(None)
        except Exception:
            pass

        try:
            mixer.master_strip().set_volume_control(None)
            mixer.set_prehear_volume_control(None)
            mixer.master_strip().set_pan_control(None)
        except Exception:
            pass

        try:
            for index in range(12):
                strip = mixer.return_strip(index)
                strip.set_volume_control(None)
                strip.set_mute_button(None)
                strip.set_solo_button(None)
                strip.set_send_controls(None)
                strip.set_pan_control(None)
        except Exception:
            pass

    def _register_mixer_automation_status(self, midi_channel, cc, channel_type, track_index, parameter_name, send_index=None):
        self._mixer_automation_status_specs.append((midi_channel, cc, channel_type, track_index, parameter_name, send_index))
        parameter = self._mixer_parameter(channel_type, track_index, parameter_name, send_index)
        if parameter and liveobj_valid(parameter) and hasattr(parameter, 'add_automation_state_listener'):
            listener = self._create_mixer_automation_state_listener()
            self._mixer_automation_state_listeners.append((parameter, listener))
            if not hasattr(parameter, 'automation_state_has_listener') or not parameter.automation_state_has_listener(listener):
                parameter.add_automation_state_listener(listener)

    def _create_mixer_automation_state_listener(self):
        def listener():
            self._schedule_mixer_automation_status_resends()
        return listener

    def _remove_mixer_automation_state_listeners(self):
        for parameter, listener in self._mixer_automation_state_listeners:
            try:
                if liveobj_valid(parameter) and hasattr(parameter, 'remove_automation_state_listener'):
                    if not hasattr(parameter, 'automation_state_has_listener') or parameter.automation_state_has_listener(listener):
                        parameter.remove_automation_state_listener(listener)
            except Exception:
                pass
        self._mixer_automation_state_listeners = []

    def _cancel_mixer_automation_status_resends(self):
        for timer in list(getattr(self, '_mixer_automation_status_timers', [])):
            try:
                timer.cancel()
            except Exception:
                pass
        self._mixer_automation_status_timers = []

    def _schedule_mixer_automation_status_resends(self, send_now=True):
        self._cancel_mixer_automation_status_resends()
        if send_now:
            self._send_mixer_automation_statuses()
        if not getattr(self, 'mixer_status', False):
            return
        for delay in (0.05, 0.15, 0.35):
            timer = threading.Timer(delay, self._send_mixer_automation_statuses)
            self._mixer_automation_status_timers.append(timer)
            timer.start()

    def _mixer_automation_state_for_parameter(self, parameter):
        try:
            if parameter and liveobj_valid(parameter) and hasattr(parameter, 'automation_state'):
                return int(parameter.automation_state)
        except Exception:
            pass
        return 0

    def _send_mixer_automation_statuses(self):
        if not self.mixer_status:
            self._send_sys_ex_message("", 0x2A)
            return

        entries = []
        for midi_channel, cc, channel_type, track_index, parameter_name, send_index in self._mixer_automation_status_specs:
            parameter = self._mixer_parameter(channel_type, track_index, parameter_name, send_index)
            state = self._mixer_automation_state_for_parameter(parameter)
            entries.append("{}:{}:{}".format(midi_channel, cc, state))
        self._send_sys_ex_message(",".join(entries), 0x2A)

    def _mixer_track(self, channel_type, track_index):
        try:
            song = self.song()
            if channel_type == "track":
                return song.tracks[track_index]
            elif channel_type == "return":
                return song.return_tracks[track_index]
            return song.master_track
        except Exception:
            return None

    def _mixer_parameter(self, channel_type, track_index, parameter_name, send_index=None):
        try:
            track = self._mixer_track(channel_type, track_index)
            if track is None:
                return None
            mixer_device = track.mixer_device
            if parameter_name == "send":
                sends = mixer_device.sends
                if send_index is not None and send_index < len(sends):
                    return sends[send_index]
                return None
            return getattr(mixer_device, parameter_name, None)
        except Exception:
            return None

    def _on_mixer_automation_control_value(self, value, channel_type, track_index, parameter_name, send_index=None):
        self._schedule_mixer_automation_status_resends(send_now=False)
        if not self._pending_parameter_automation_action():
            return

        parameter = self._mixer_parameter(channel_type, track_index, parameter_name, send_index)
        track = self._mixer_track(channel_type, track_index)
        self._consume_remove_automation_request(parameter, track)
        
    def _set_up_mixer_controls(self):
        song = self.song()
        tracks = song.tracks
        return_tracks = song.return_tracks
        hidden_track_states = self._group_hidden_state_codes(tracks)
        visible_track_indexes = set(
            index for index, hidden in enumerate(hidden_track_states)
            if hidden != "1"
        )
        self._remove_mixer_automation_state_listeners()
        self._release_mixer_controls()
        self._disconnect_mixer_component_controls()
        self._mixer_automation_status_specs = []
        first_visible_channel = self.visible_channels[0]
        last_visible_channel = self.visible_channels[1]
        master_track_index = len(tracks) + len(return_tracks)
        master_track_visible = first_visible_channel <= master_track_index <= last_visible_channel
        meter_targets = {}
        if self.mixer_status:
            for index, track in enumerate(tracks):
                if index in visible_track_indexes and index >= first_visible_channel and index <= last_visible_channel:
                    meter_targets[index] = track
            for index, return_track in enumerate(return_tracks):
                combined_index = len(tracks) + index
                if combined_index >= first_visible_channel and combined_index <= last_visible_channel:
                    meter_targets[combined_index] = return_track
            if master_track_visible:
                meter_targets[127] = song.master_track
        self._mixer_meter_targets = meter_targets
        
        # Channels
        for index, track in enumerate(tracks):

            if index in meter_targets:
                self._create_mixer_automation_control("slider", 2, index, "track", index, "volume")
                self._create_mixer_automation_control("encoder", 3, index, "track", index, "send", 0)
                self._create_mixer_automation_control("encoder", 4, index, "track", index, "send", 1)
                self._create_mixer_automation_control("encoder", 5, index, "track", index, "panning")
                self._create_mixer_toggle_control(6, index, "track", index, "mute")
                self._create_mixer_toggle_control(7, index, "track", index, "solo")
                # reseting volume just in case
                self._on_output_level_changed(index)

            # Other strip controls can be configured similarly
            # strip.set_arm_button(...)
            # strip.set_shift_button(...)

        # Master / channel 7 cc 127
        if 127 in meter_targets:
            self._create_mixer_automation_control("slider", 0, 127, "master", 0, "volume")
            self._create_mixer_automation_control("encoder", 0, 126, "master", 0, "cue_volume")
            self._create_mixer_automation_control("encoder", 0, 125, "master", 0, "panning")
            # reseting volume just in case
            self._on_output_level_changed(127)

        # Return Tracks
        for index, returnTrack in enumerate(return_tracks):
            combined_index = len(tracks) + index
            if combined_index in meter_targets:
                self._create_mixer_automation_control("slider", 8, index, "return", index, "volume")
                self._create_mixer_toggle_control(8, index + 12, "return", index, "mute")
                self._create_mixer_toggle_control(8, index + 24, "return", index, "solo")
                self._create_mixer_automation_control("encoder", 8, index + 36, "return", index, "send", 0)
                self._create_mixer_automation_control("encoder", 8, index + 48, "return", index, "send", 1)
                self._create_mixer_automation_control("encoder", 8, index + 60, "return", index, "panning")
                # reseting volume just in case
                self._on_output_level_changed(index + len(tracks))

        self._schedule_mixer_automation_status_resends()
        
    def _on_output_level_changed(self, index):
        if self.mixer_status:
            if not self.mixer_reset:
                self.mixer_reset = True
            track = self._mixer_meter_targets.get(index)
            if track and liveobj_valid(track):
                if track.has_audio_output:
                    left_channel = track.output_meter_left
                    right_channel = track.output_meter_right
                else:
                    left_channel = 0.0
                    right_channel = 0.0
    
                value_left = int(round(left_channel * 100))
                value_right = int(round(right_channel * 100))
    
                # send midi cc left on channel 9, right on channel 10, cc == index, 
                # value == Int(left_channel * 100)
    
                status_byte_left = 0xB8 | 9  # MIDI CC message on channel 9
                midi_cc_message_left = (status_byte_left, index, value_left)
                self._send_midi(midi_cc_message_left)
                status_byte_right = 0xB8 | 10  # MIDI CC message on channel 10
                midi_cc_message_right = (status_byte_right, index, value_right)
                self._send_midi(midi_cc_message_right)

        elif self.mixer_reset:
            self.mixer_reset = False

            song = self.song()
            tracks = song.tracks
            return_tracks = song.return_tracks
            value = 0
            total_track_number = len(tracks) + len(return_tracks) + 1

            for index in range(total_track_number):
                status_byte_left = 0xB8 | 9  # MIDI CC message on channel 9
                midi_cc_message_left = (status_byte_left, index, value)
                self._send_midi(midi_cc_message_left)
                status_byte_right = 0xB8 | 10  # MIDI CC message on channel 10
                midi_cc_message_right = (status_byte_right, index, value)
                self._send_midi(midi_cc_message_right)

    def _get_track_index(self, track):
        if not track:
            return None
        try:
            return list(self.song().tracks).index(track)
        except ValueError:
            return None

    def _make_clip_has_clip_listener(self, track):
        def listener():
            self._on_clip_has_clip_changed(track)
        return listener

    def _make_clip_triggered_listener(self, track):
        def listener():
            self._on_clip_playing_status_changed(track)
        return listener

    def _make_clip_playing_listener(self, track):
        def listener():
            self._on_clip_playing_status_changed(track)
        return listener

    def _make_clip_color_listener(self, track):
        def listener():
            self._on_clip_has_clip_changed(track)
        return listener

    def _sync_clip_color_listeners_for_track(self, track):
        for clip_slot in track.clip_slots:
            if clip_slot is None:
                continue
            if clip_slot.has_clip:
                current_clip = clip_slot.clip
                previous_clip = self._clip_slot_color_map.get(clip_slot)
                if previous_clip is not None and previous_clip != current_clip:
                    old_listener = self._clip_color_listeners.pop(previous_clip, None)
                    if old_listener and liveobj_valid(previous_clip) and previous_clip.color_has_listener(old_listener):
                        previous_clip.remove_color_listener(old_listener)
                if current_clip not in self._clip_color_listeners:
                    listener = self._make_clip_color_listener(track)
                    self._clip_color_listeners[current_clip] = listener
                    current_clip.add_color_listener(listener)
                self._clip_slot_color_map[clip_slot] = current_clip
            else:
                previous_clip = self._clip_slot_color_map.pop(clip_slot, None)
                if previous_clip is not None:
                    old_listener = self._clip_color_listeners.pop(previous_clip, None)
                    if old_listener and liveobj_valid(previous_clip) and previous_clip.color_has_listener(old_listener):
                        previous_clip.remove_color_listener(old_listener)

    # clipSlots
    def _register_clip_listeners(self):
        current_track_ids = set()
        for track in self.song().tracks:
            track_id = id(track)
            current_track_ids.add(track_id)

            for clip_slot in track.clip_slots:

                if clip_slot == None:
                    continue

                listener_key = (clip_slot, 'has_clip')
                if listener_key not in self._clip_slot_listeners:
                    listener = self._make_clip_has_clip_listener(track)
                    self._clip_slot_listeners[listener_key] = listener
                    clip_slot.add_has_clip_listener(listener)

                listener_key = (clip_slot, 'is_triggered')
                if listener_key not in self._clip_slot_listeners:
                    listener = self._make_clip_triggered_listener(track)
                    self._clip_slot_listeners[listener_key] = listener
                    clip_slot.add_is_triggered_listener(listener)

                listener_key = (clip_slot, 'is_playing')
                if listener_key not in self._clip_slot_listeners:
                    try:
                        listener = self._make_clip_playing_listener(track)
                        self._clip_slot_listeners[listener_key] = listener
                        clip_slot.add_is_playing_listener(listener)
                    except Exception:
                        pass
            
            self._registered_track_ids.add(track_id)
        
        # Clean up stale entries for tracks that no longer exist
        self._registered_track_ids &= current_track_ids
        
        # Sync color listeners for all tracks
        for track in self.song().tracks:
            self._sync_clip_color_listeners_for_track(track)

    def _unregister_clip_and_audio_listeners(self):
        for track in self.song().tracks:
            for clip_slot in track.clip_slots:
                listener_key = (clip_slot, 'is_triggered')
                listener = self._clip_slot_listeners.pop(listener_key, None)
                if listener:
                    clip_slot.remove_is_triggered_listener(listener)
                else:
                    clip_slot.remove_is_triggered_listener(self._on_clip_playing_status_changed)

                listener_key = (clip_slot, 'is_playing')
                listener = self._clip_slot_listeners.pop(listener_key, None)
                if listener:
                    try:
                        clip_slot.remove_is_playing_listener(listener)
                    except Exception:
                        pass
                
                listener_key = (clip_slot, 'has_clip')
                listener = self._clip_slot_listeners.pop(listener_key, None)
                if listener:
                    clip_slot.remove_has_clip_listener(listener)
                else:
                    clip_slot.remove_has_clip_listener(self._on_clip_has_clip_changed)
                
                if clip_slot.has_clip:
                    listener = self._clip_color_listeners.pop(clip_slot.clip, None)
                    if listener:
                        clip_slot.clip.remove_color_listener(listener)
                    else:
                        clip_slot.clip.remove_color_listener(self._on_clip_has_clip_changed)
                # if clip_slot.has_clip:
                #     # clip_slot.clip.remove_playing_status_listener(self._on_clip_playing_status_changed)
                #     clip_slot.clip.remove_playing_position_listener(self._on_playing_position_changed)
            # output meter listeners - use stored handler references
            if track in self._track_level_listeners:
                left_listener, right_listener = self._track_level_listeners[track]
                if liveobj_valid(track) and hasattr(track, 'output_meter_left_has_listener') and track.output_meter_left_has_listener(left_listener):
                    track.remove_output_meter_left_listener(left_listener)
                if liveobj_valid(track) and hasattr(track, 'output_meter_right_has_listener') and track.output_meter_right_has_listener(right_listener):
                    track.remove_output_meter_right_listener(right_listener)

        for return_track, (left_listener, right_listener) in self._return_level_listeners.items():
            if liveobj_valid(return_track) and hasattr(return_track, 'output_meter_left_has_listener') and return_track.output_meter_left_has_listener(left_listener):
                return_track.remove_output_meter_left_listener(left_listener)
            if liveobj_valid(return_track) and hasattr(return_track, 'output_meter_right_has_listener') and return_track.output_meter_right_has_listener(right_listener):
                return_track.remove_output_meter_right_listener(right_listener)
        
        self._track_level_listeners.clear()
        self._return_level_listeners.clear()
        self._clip_listener_track_slots.clear()
        self._clip_slot_listeners.clear()
        self._clip_color_listeners.clear()
        self._clip_slot_color_map.clear()

    # def _on_playing_position_changed(self):
    #     # self.log_message("trying to log the playing position")
    #     self._update_clip_slots()

    def find_different_indexes(self, arrays1, arrays2):
        different_indexes = []

        for index, (array1, array2) in enumerate(zip_longest(arrays1, arrays2)):
            if array1 != array2:
                different_indexes.append(index)

        return different_indexes

    def _on_clip_playing_status_changed(self, track=None):
        # self.log_message("clip playing status changed")
        self._refresh_parameter_metadata_on_automation_change()
        if track:
            track_index = self._get_track_index(track)
            if track_index is not None:
                self._update_clip_slots(track_index)
                self._activate_follow_actions_for_playing_clips(track_index)
                self._evaluate_mutator_regeneration(track_index)
                return
        self._update_clip_slots()
        self._activate_follow_actions_for_playing_clips()
        self._evaluate_mutator_regeneration()

    def _activate_follow_actions_for_playing_clips(self, only_track_index=None):
        self._clear_finished_follow_action_launches()
        try:
            for track_index, track in enumerate(self.song().tracks):
                if only_track_index is not None and track_index != only_track_index:
                    continue
                for scene_index, clip_slot in enumerate(track.clip_slots):
                    if not clip_slot.has_clip or not clip_slot.is_playing:
                        continue
                    key = self._follow_action_key("clip", track_index, scene_index)
                    if key in self._active_follow_actions:
                        continue
                    if key in self._follow_action_rules:
                        self._activate_follow_action_for_clip(track_index, scene_index, clip_slot)
        except Exception:
            pass

    def _make_follow_action_scene_triggered_listener(self, scene_index):
        def listener():
            try:
                key = self._follow_action_key("scene", None, scene_index)
                if key not in self._follow_action_rules:
                    return
                scene = self.song().scenes[scene_index]
                if self._scene_object_is_triggered(scene) or self._scene_is_launched(scene_index):
                    self._activate_follow_action_for_scene(scene_index)
            except Exception:
                pass
        return listener

    def _make_follow_action_clip_slot_listener(self, track_index, scene_index, clip_slot):
        def listener():
            try:
                key = self._follow_action_key("clip", track_index, scene_index)
                if key not in self._follow_action_rules:
                    return
                if clip_slot.has_clip and (clip_slot.is_playing or self._clip_slot_is_triggered(clip_slot)):
                    self._activate_follow_action_for_clip(track_index, scene_index, clip_slot)
            except Exception:
                pass
        return listener

    def _make_follow_action_name_listener(self):
        def listener():
            self._on_follow_action_name_changed()
        return listener

    def _make_follow_action_clip_timing_listener(self):
        def listener():
            self._on_follow_action_timing_changed()
        return listener

    def _make_follow_action_clip_has_clip_listener(self):
        def listener():
            self._sync_follow_action_name_listeners()
            self._load_follow_actions_from_names()
            self._sync_follow_action_runtime_listeners()
        return listener

    def _remove_named_object_listener(self, listener_map, key):
        listener_info = listener_map.pop(key, None)
        if not listener_info:
            return
        named_object, listener = listener_info
        try:
            remove_listener = getattr(named_object, "remove_name_listener", None)
            has_listener = getattr(named_object, "name_has_listener", None)
            if remove_listener and (not has_listener or has_listener(listener)):
                remove_listener(listener)
        except Exception:
            pass

    def _remove_follow_action_clip_timing_listeners(self, key):
        listener_info = self._follow_action_clip_timing_listeners.pop(key, None)
        if not listener_info:
            return
        clip, listeners = listener_info
        for property_name, listener in listeners.items():
            try:
                remove_listener = getattr(clip, "remove_{}_listener".format(property_name), None)
                has_listener = getattr(clip, "{}_has_listener".format(property_name), None)
                if remove_listener and (not has_listener or has_listener(listener)):
                    remove_listener(listener)
            except Exception:
                pass

    def _ensure_follow_action_clip_timing_listeners(self, key, clip):
        existing = self._follow_action_clip_timing_listeners.get(key)
        if existing and existing[0] is clip:
            return

        self._remove_follow_action_clip_timing_listeners(key)
        listeners = {}
        for property_name in ("loop_start", "loop_end", "end_marker"):
            try:
                add_listener = getattr(clip, "add_{}_listener".format(property_name), None)
                has_listener = getattr(clip, "{}_has_listener".format(property_name), None)
                if not add_listener:
                    continue
                listener = self._make_follow_action_clip_timing_listener()
                if not has_listener or not has_listener(listener):
                    add_listener(listener)
                listeners[property_name] = listener
            except Exception:
                pass

        if listeners:
            self._follow_action_clip_timing_listeners[key] = (clip, listeners)

    def _clip_affects_follow_action_timing(self, track_index, scene_index):
        return (
            self._follow_action_key("clip", track_index, scene_index) in self._follow_action_rules
            or self._follow_action_key("scene", None, scene_index) in self._follow_action_rules
        )

    def _remove_follow_action_clip_has_clip_listener(self, key):
        listener_info = self._follow_action_clip_has_clip_listeners.pop(key, None)
        if not listener_info:
            return
        clip_slot, listener = listener_info
        try:
            remove_listener = getattr(clip_slot, "remove_has_clip_listener", None)
            has_listener = getattr(clip_slot, "has_clip_has_listener", None)
            if remove_listener and (not has_listener or has_listener(listener)):
                remove_listener(listener)
        except Exception:
            pass

    def _sync_follow_action_name_listeners(self):
        expected_scene_keys = set()
        expected_clip_keys = set()
        expected_clip_slot_keys = set()
        expected_clip_timing_keys = set()

        try:
            scenes = self.song().scenes
            for scene in scenes:
                key = self._live_object_identity(scene)
                expected_scene_keys.add(key)
                existing = self._follow_action_scene_name_listeners.get(key)
                if existing and existing[0] is scene:
                    continue
                self._remove_named_object_listener(self._follow_action_scene_name_listeners, key)
                try:
                    listener = self._make_follow_action_name_listener()
                    add_listener = getattr(scene, "add_name_listener", None)
                    has_listener = getattr(scene, "name_has_listener", None)
                    if add_listener and (not has_listener or not has_listener(listener)):
                        add_listener(listener)
                    self._follow_action_scene_name_listeners[key] = (scene, listener)
                except Exception:
                    pass

            for track_index, track in enumerate(self.song().tracks):
                for scene_index, clip_slot in enumerate(track.clip_slots):
                    slot_key = self._live_object_identity(clip_slot)
                    expected_clip_slot_keys.add(slot_key)
                    existing_slot = self._follow_action_clip_has_clip_listeners.get(slot_key)
                    if not existing_slot or existing_slot[0] is not clip_slot:
                        self._remove_follow_action_clip_has_clip_listener(slot_key)
                        try:
                            listener = self._make_follow_action_clip_has_clip_listener()
                            add_listener = getattr(clip_slot, "add_has_clip_listener", None)
                            has_listener = getattr(clip_slot, "has_clip_has_listener", None)
                            if add_listener and (not has_listener or not has_listener(listener)):
                                add_listener(listener)
                            self._follow_action_clip_has_clip_listeners[slot_key] = (clip_slot, listener)
                        except Exception:
                            pass

                    if not clip_slot.has_clip:
                        continue
                    clip = clip_slot.clip
                    clip_key = self._live_object_identity(clip)
                    expected_clip_keys.add(clip_key)
                    existing_clip = self._follow_action_clip_name_listeners.get(clip_key)
                    needs_timing_listener = self._clip_affects_follow_action_timing(track_index, scene_index)
                    if needs_timing_listener:
                        expected_clip_timing_keys.add(clip_key)
                    else:
                        self._remove_follow_action_clip_timing_listeners(clip_key)
                    if existing_clip and existing_clip[0] is clip:
                        if needs_timing_listener:
                            self._ensure_follow_action_clip_timing_listeners(clip_key, clip)
                        continue
                    self._remove_named_object_listener(self._follow_action_clip_name_listeners, clip_key)
                    try:
                        listener = self._make_follow_action_name_listener()
                        add_listener = getattr(clip, "add_name_listener", None)
                        has_listener = getattr(clip, "name_has_listener", None)
                        if add_listener and (not has_listener or not has_listener(listener)):
                            add_listener(listener)
                        self._follow_action_clip_name_listeners[clip_key] = (clip, listener)
                    except Exception:
                        pass
                    if needs_timing_listener:
                        self._ensure_follow_action_clip_timing_listeners(clip_key, clip)
        except Exception:
            pass

        for key in list(self._follow_action_scene_name_listeners.keys()):
            if key not in expected_scene_keys:
                self._remove_named_object_listener(self._follow_action_scene_name_listeners, key)
        for key in list(self._follow_action_clip_name_listeners.keys()):
            if key not in expected_clip_keys:
                self._remove_named_object_listener(self._follow_action_clip_name_listeners, key)
        for key in list(self._follow_action_clip_has_clip_listeners.keys()):
            if key not in expected_clip_slot_keys:
                self._remove_follow_action_clip_has_clip_listener(key)
        for key in list(self._follow_action_clip_timing_listeners.keys()):
            if key not in expected_clip_timing_keys:
                self._remove_follow_action_clip_timing_listeners(key)

    def _remove_follow_action_name_listeners(self):
        for key in list(self._follow_action_scene_name_listeners.keys()):
            self._remove_named_object_listener(self._follow_action_scene_name_listeners, key)
        for key in list(self._follow_action_clip_name_listeners.keys()):
            self._remove_named_object_listener(self._follow_action_clip_name_listeners, key)
        for key in list(self._follow_action_clip_has_clip_listeners.keys()):
            self._remove_follow_action_clip_has_clip_listener(key)
        for key in list(self._follow_action_clip_timing_listeners.keys()):
            self._remove_follow_action_clip_timing_listeners(key)

    def _remove_follow_action_scene_listener(self, key):
        listener_info = self._follow_action_scene_triggered_listeners.pop(key, None)
        if not listener_info:
            return
        scene, listener = listener_info
        try:
            remove_listener = getattr(scene, "remove_is_triggered_listener", None)
            has_listener = getattr(scene, "is_triggered_has_listener", None)
            if remove_listener and (not has_listener or has_listener(listener)):
                remove_listener(listener)
        except Exception:
            pass

    def _remove_follow_action_clip_slot_listeners(self, key):
        listener_info = self._follow_action_clip_slot_runtime_listeners.pop(key, None)
        if not listener_info:
            return
        clip_slot, triggered_listener, playing_listener = listener_info
        try:
            remove_listener = getattr(clip_slot, "remove_is_triggered_listener", None)
            has_listener = getattr(clip_slot, "is_triggered_has_listener", None)
            if remove_listener and (not has_listener or has_listener(triggered_listener)):
                remove_listener(triggered_listener)
        except Exception:
            pass
        try:
            remove_listener = getattr(clip_slot, "remove_is_playing_listener", None)
            has_listener = getattr(clip_slot, "is_playing_has_listener", None)
            if remove_listener and (not has_listener or has_listener(playing_listener)):
                remove_listener(playing_listener)
        except Exception:
            pass

    def _sync_follow_action_runtime_listeners(self):
        expected_scene_keys = set()
        expected_clip_keys = set()

        for key, rule in self._follow_action_rules.items():
            target_kind = rule.get("target_kind")
            scene_index = rule.get("scene_index")

            if target_kind == "scene":
                expected_scene_keys.add(key)
                try:
                    scene = self.song().scenes[scene_index]
                except Exception:
                    continue
                existing = self._follow_action_scene_triggered_listeners.get(key)
                if existing and existing[0] is scene:
                    continue
                self._remove_follow_action_scene_listener(key)
                try:
                    listener = self._make_follow_action_scene_triggered_listener(scene_index)
                    scene.add_is_triggered_listener(listener)
                    self._follow_action_scene_triggered_listeners[key] = (scene, listener)
                except Exception:
                    pass

            elif target_kind == "clip":
                track_index = rule.get("track_index")
                expected_clip_keys.add(key)
                try:
                    clip_slot = self.song().tracks[track_index].clip_slots[scene_index]
                except Exception:
                    continue
                existing = self._follow_action_clip_slot_runtime_listeners.get(key)
                if existing and existing[0] is clip_slot:
                    continue
                self._remove_follow_action_clip_slot_listeners(key)
                try:
                    triggered_listener = self._make_follow_action_clip_slot_listener(track_index, scene_index, clip_slot)
                    playing_listener = self._make_follow_action_clip_slot_listener(track_index, scene_index, clip_slot)
                    clip_slot.add_is_triggered_listener(triggered_listener)
                    clip_slot.add_is_playing_listener(playing_listener)
                    self._follow_action_clip_slot_runtime_listeners[key] = (clip_slot, triggered_listener, playing_listener)
                except Exception:
                    pass

        for key in list(self._follow_action_scene_triggered_listeners.keys()):
            if key not in expected_scene_keys:
                self._remove_follow_action_scene_listener(key)
        for key in list(self._follow_action_clip_slot_runtime_listeners.keys()):
            if key not in expected_clip_keys:
                self._remove_follow_action_clip_slot_listeners(key)

    def _remove_follow_action_runtime_listeners(self):
        for key in list(self._follow_action_scene_triggered_listeners.keys()):
            self._remove_follow_action_scene_listener(key)
        for key in list(self._follow_action_clip_slot_runtime_listeners.keys()):
            self._remove_follow_action_clip_slot_listeners(key)

    def _clear_finished_follow_action_launches(self):
        for key in list(self._handled_follow_action_launches):
            try:
                if key[0] == "clip":
                    _, track_index, scene_index = key
                    clip_slot = self.song().tracks[track_index].clip_slots[scene_index]
                    if not clip_slot.has_clip or not clip_slot.is_playing:
                        self._handled_follow_action_launches.discard(key)
                elif key[0] == "scene":
                    _, scene_index = key
                    if not self._scene_is_playing(scene_index):
                        self._handled_follow_action_launches.discard(key)
            except Exception:
                self._handled_follow_action_launches.discard(key)

    def _on_clip_has_clip_changed(self, track=None):
        # self.log_message("has clip status changed")
        self._refresh_parameter_metadata_on_automation_change()
        if track:
            track_index = self._get_track_index(track)
            if track_index is not None:
                self._update_clip_slots(track_index)
                self._sync_clip_color_listeners_for_track(track)
                self._set_up_notes_playing("clip")
                return
        self._update_clip_slots()
        self._set_up_notes_playing("clip")

    def _update_clip_slots(self, only_track_index=None):
        try:
            track_clips = []
            tracks = self.song().tracks
            if only_track_index is not None and len(self.old_clips_array) != len(tracks):
                only_track_index = None
            for track_index, track in enumerate(tracks):
                if only_track_index is not None and track_index != only_track_index:
                    track_clips.append(self.old_clips_array[track_index] if track_index < len(self.old_clips_array) else "")
                    continue
                try:
                    is_armed = track.arm
                    has_audio = track.has_audio_input
                except Exception:
                    is_armed = False
                    has_audio = False
                # track clip slots
                clip_slots = []
                try:
                    for clip_slot in track.clip_slots:
                        clip_value = "0"
                        try:
                            if clip_slot.is_triggered:
                                clip_value = "4"
                            elif clip_slot.is_recording:
                                clip_value = "3"
                            elif clip_slot.is_playing:
                                clip_value = "2"
                            elif clip_slot.has_clip:
                                clip_value = "1"
                            elif is_armed and has_audio:
                                clip_value = "5"
                        except Exception:
                            clip_value = "0"

                        color_string_value = "0"
                        
                        # this could also just be made to if value == "1", but does not hurt this way
                        if clip_value != "0" and clip_slot.has_clip:
                            # extra test if has clip because group channels don't have a clip but might be triggered etc
                            try:
                                if clip_slot.clip.color is not None:
                                    color_string_value = self._make_color_string(clip_slot.clip.color)
                            except Exception:
                                color_string_value = "0"
                        #     playing_position = clip_slot.clip.playing_position
                        #     length = clip_slot.clip.length
                        #     self.log_message("playing: {} triggering {}".format(is_playing_value, is_triggered_value))
                        # else:
                        #     playing_position = 0.0
                        #     length = 0.0

                        clip_string = "{}:{}".format(clip_value, color_string_value)
                        clip_slots.append(clip_string)
                except Exception as e:
                    pass
                clip_slots_string = "-".join(clip_slots)
                track_clips.append(clip_slots_string)

            # compare old track clips with new
            clips_difference = self.find_different_indexes(track_clips, self.old_clips_array)
            
            # safe new values
            self.old_clips_array = track_clips

            # send different tracks out
            # TODO: now this still does send out each track. not sure why. also it should only send out the clip that is changed and not the whole track I would say.
            if clips_difference != []:
                for track_index in clips_difference:
                    if int(track_index) < len(track_clips):
                        string_prefix = str(track_index) + "%"
                        track_string = string_prefix + str(track_clips[track_index])
                        self._send_sys_ex_message(track_string, 0x05)
                    else:
                        delete_clips = "DEL" + str(track_index)
                        self._send_sys_ex_message(delete_clips, 0x05)
        except Exception as e:
            # need to stop threading or we get a fatal error.
            # self.periodic_timer = 0
            pass

    def _on_scale_changed(self):
        song = self.song()
        scale = song.scale_name
        root = song.root_note
        scale_string = "{};{}".format(self._escape_sysex_string(scale), root)
        self._send_sys_ex_message(scale_string, 0x0A)
        self._schedule_mutator_scale_root_sync()
        try:
            self.send_selected_clip_metadata()
        except Exception:
            pass

    def handle_sysex(self, message):
        """
        Handles incoming SysEx messages, including multi-part (chunked) ones.
        Chunks start with '$' (more coming) or '_' (final chunk).
        Only for message ids that can be chunked by the app.
        """
        # Ensure we have a buffer for assembling chunks
        if not hasattr(self, "_sysex_buffer"):
            self._sysex_buffer = []
    
        # Basic validity check
        if len(message) < 3:
            self._sysex_buffer = []
            return
        
        manufacturer_id = message[1]
        prefix = message[2]
    
        # Check if this message is chunked. Existing Tap app -> script chunks
        # always use the prefix directly after the manufacturer id.
        if manufacturer_id in self.CHUNKED_INCOMING_SYSEX_IDS:
            if prefix == 36:
                # Intermediate chunk
                self._sysex_buffer.extend(message[3:-1])  # skip F0, manuf, prefix, F7
                return
        
            elif prefix == 95:
                # Final chunk — assemble full message
                self._sysex_buffer.extend(message[3:-1])
                full_message = [0xF0, manufacturer_id] + self._sysex_buffer + [0xF7]
                self._sysex_buffer = []  # reset buffer
        
                # Now call the original handler
                self._handle_full_sysex(full_message)
                return
            
            else:
                self._sysex_buffer = []
                self._handle_full_sysex(message)
                return
        
        else:
            if self._sysex_buffer != []:
                # Cancel chunking, something went wrong
                self._sysex_buffer = []
                return
                
            else:
                # Non-chunked message → handle directly
                self._handle_full_sysex(message)

    def _handle_full_sysex(self, message):
        # start stop clip
        if len(message) >= 2 and message[1] == 9:
            values = self.extract_values_from_sysex_message(message)
            if len(values) == 3:
                self._fire_clip(values[0], values[1], values[2])
        # delete clip
        if len(message) >= 2 and message[1] == 10:
            values = self.extract_values_from_sysex_message(message)
            if len(values) == 2:
                self._delete_clip(values[0], values[1])
        # copy paste clip
        if len(message) >= 2 and message[1] == 11:
            values = self.extract_values_from_sysex_message(message)
            if len(values) == 4:
                self._copy_paste_clip(values[0], values[1], values[2], values[3])
        # scale and rootnote
        if len(message) >= 2 and message[1] == 12:
            values = self.decode_sys_ex_scale_root(message)
            if len(values) == 2:
                self._set_scale_root_note(values[0], values[1])
        # duplicate loop
        if len(message) >= 2 and message[1] == 13:
            values = self.extract_values_from_sysex_message(message)
            if len(values) == 2:
                self._duplicate_loop(values[0], values[1])
        # request selected track local controls (ModWheel/Pressure)
        if len(message) >= 2 and message[1] == 59:
            self._send_track_local_control_state()
        
        # add MULTIPLE notes
        if len(message) >= 2 and message[1] == 14:
            index = 2
            new_notes = []
            new_note_values = []

            # Decode all notes in the message
            while index < (len(message) - 1):
                # Decode the pitch of the note
                note_pitch = message[index]
                index += 1

                # Decode the start time (3 bytes, 7-bit packed)
                if index + 3 > len(message):
                    break
                start_time = self._from_3_7bit_bytes(message, index)
                index += 3

                # Decode the duration (3 bytes, 7-bit packed)
                if index + 3 > len(message):
                    break
                duration = self._from_3_7bit_bytes(message, index)
                index += 3

                # Decode velocity
                if index >= len(message):
                    break
                velocity = message[index]
                index += 1

                # Decode mute and probability
                if index >= len(message):
                    break
                mute_and_probability = message[index]
                mute = (mute_and_probability & 0x80) != 0
                probability = (mute_and_probability & 0x7F) / 127.0
                index += 1

                # Create a MidiNoteSpecification object
                note_spec = MidiNoteSpecification(
                    pitch=note_pitch,
                    start_time=start_time / 1000.0,
                    duration=duration / 1000.0,
                    velocity=velocity,
                    mute=mute,
                    probability=probability
                )

                new_notes.append(note_spec)
                new_note_values.append((
                    note_pitch,
                    start_time / 1000.0,
                    duration / 1000.0,
                    velocity,
                    mute,
                    probability
                ))

            # Add all decoded notes to the current clip
            song = self.song()
            clip_slot = song.view.highlighted_clip_slot
            if clip_slot is not None and clip_slot.has_clip and len(new_notes) > 0:
                clip = clip_slot.clip
                filtered_notes = []
                filtered_note_values = []
                for note_spec, note_values in zip(new_notes, new_note_values):
                    if self._mutator_allows_source_note_time(clip, note_values[1]):
                        filtered_notes.append(note_spec)
                        filtered_note_values.append(note_values)
                new_notes = filtered_notes
                new_note_values = filtered_note_values
                if not new_notes:
                    return
                decoupled_info = self._decoupled_automation_info(clip)
                if decoupled_info:
                    repeated_notes = []
                    for pitch, start_time, duration, velocity, mute, probability in new_note_values:
                        repeated_notes.extend(self._make_repeated_note_specs_from_values(
                            pitch,
                            start_time,
                            duration,
                            velocity,
                            mute,
                            probability,
                            decoupled_info
                        ))
                    if repeated_notes:
                        clip.add_new_notes(repeated_notes)
                else:
                    clip.add_new_notes(new_notes)
        
        # remove note (also multiple)
        if len(message) >= 2 and message[1] == 15:
            note_ids = []
            index = 2
            while index < (len(message) - 1):
                note_id = message[index] | (message[index + 1] << 7)
                note_ids.append(note_id)
                index += 2
        
            # Get the selected clip
            song = self.song()
            clip_slot = song.view.highlighted_clip_slot
            if clip_slot is not None and clip_slot.has_clip:
                clip = clip_slot.clip
                decoupled_info = self._decoupled_automation_info(clip)
                if decoupled_info:
                    notes = clip.get_notes_extended(0, 128, decoupled_info["note_start"], decoupled_info["physical_length"])
                    note_ids = [
                        note_id for note_id in note_ids
                        if any(note.note_id == note_id and self._mutator_allows_source_note_time(clip, note.start_time) for note in notes)
                    ]
                    if not note_ids:
                        return
                    remove_ids = set(note_ids)
                    targets = []
                    for note in notes:
                        if note.note_id in remove_ids:
                            targets.append((int(note.pitch), self._folded_note_time(note.start_time, decoupled_info)))
                    matching_ids = []
                    for note in notes:
                        for pitch, folded_time in targets:
                            if self._note_matches_folded_time(note, folded_time, decoupled_info, pitch=pitch):
                                matching_ids.append(note.note_id)
                                break
                    if matching_ids:
                        clip.remove_notes_by_id(tuple(matching_ids))
                else:
                    clip_start = min(clip.start_time, clip.start_marker, clip.loop_start) - self.clip_length_trick
                    clip_length = (max(clip.loop_end, clip.end_marker, clip.length) + self.clip_length_trick) - clip_start
                    notes = clip.get_notes_extended(0, 128, clip_start, clip_length)
                    note_ids = [
                        note_id for note_id in note_ids
                        if any(note.note_id == note_id and self._mutator_allows_source_note_time(clip, note.start_time) for note in notes)
                    ]
                    if not note_ids:
                        return
                    # Remove the note by ID
                    clip.remove_notes_by_id(note_ids)
        
        # modify MULTIPLE notes
        if len(message) >= 3 and message[1] == 16:
            index = 2
        
            # Get the selected clip
            song = self.song()
            clip_slot = song.view.highlighted_clip_slot
            if clip_slot is not None and clip_slot.has_clip:
                clip = clip_slot.clip
                decoupled_info = self._decoupled_automation_info(clip)
                
                # Fetch existing notes from the clip
                clip_start = min(clip.start_time, clip.start_marker, clip.loop_start) - self.clip_length_trick
                clip_length = (max(clip.loop_end, clip.end_marker, clip.length) + self.clip_length_trick) - clip_start
                notes = clip.get_notes_extended(0, 128, clip_start, clip_length)
                did_modify_notes = False
        
                # Modify the matching notes
                while index < (len(message) - 1):
                    note_id = message[index] | (message[index + 1] << 7)
                    pitch = message[index + 2]
                    index += 3
        
                    start_time_raw = self._from_3_7bit_bytes(message, index)
                    start_time = start_time_raw / 1000.0
                    index += 3
        
                    duration_raw = self._from_3_7bit_bytes(message, index)
                    duration = duration_raw / 1000.0
                    index += 3
        
                    velocity = message[index]
                    index += 1
        
                    mute = bool(message[index] & 0x80)
                    probability = (message[index] & 0x7F) / 127.0
                    index += 1

                    if decoupled_info:
                        target_note = None
                        for note in notes:
                            if note.note_id == note_id:
                                target_note = note
                                break
                        if target_note is None:
                            continue
                        if not self._mutator_allows_source_note_time(clip, target_note.start_time):
                            continue
                        if not self._mutator_allows_source_note_time(clip, start_time):
                            continue

                        old_pitch = int(target_note.pitch)
                        old_folded_time = self._folded_note_time(target_note.start_time, decoupled_info)
                        new_offset = self._positive_mod(start_time - decoupled_info["note_start"], decoupled_info["note_length"])
                        clipped_duration = self._duration_inside_note_loop(start_time, duration, decoupled_info)
                        for note in notes:
                            if self._note_matches_folded_time(note, old_folded_time, decoupled_info, pitch=old_pitch):
                                repeat_index = int(math.floor(max(0.0, note.start_time - decoupled_info["note_start"]) / decoupled_info["note_length"]))
                                note.pitch = pitch
                                note.start_time = decoupled_info["note_start"] + (float(repeat_index) * decoupled_info["note_length"]) + new_offset
                                note.duration = clipped_duration
                                note.velocity = velocity
                                note.mute = mute
                                note.probability = probability
                                did_modify_notes = True
                    else:
                        for note in notes:
                            if note.note_id == note_id:
                                if not self._mutator_allows_source_note_time(clip, note.start_time):
                                    break
                                if not self._mutator_allows_source_note_time(clip, start_time):
                                    break
                                note.pitch = pitch
                                note.start_time = start_time
                                note.duration = duration
                                note.velocity = velocity
                                note.mute = mute
                                note.probability = probability
                                did_modify_notes = True
                                break
        
                # Apply the modified notes back to the clip
                if did_modify_notes:
                    clip.apply_note_modifications(notes)
        
        # markers
        if len(message) >= 2 and message[1] == 17:
            # Decode the note ID and data
            marker_id = message[2]
            
            # Decode start time (variable-length value)
            index = 3
            marker_time_raw = self._from_3_7bit_bytes(message, index)
            marker_time = marker_time_raw / 1000.0
            
            # Get the selected clip
            song = self.song()
            clip_slot = song.view.highlighted_clip_slot
            if clip_slot is not None and clip_slot.has_clip:
                clip = clip_slot.clip
                if marker_id == 0:
                    clip.start_marker = marker_time
                elif marker_id == 1:
                    clip.end_marker = marker_time
                elif marker_id == 2:
                    decoupled_info = self._decoupled_automation_info(clip)
                    if decoupled_info:
                        note_end = decoupled_info["note_start"] + decoupled_info["note_length"]
                        note_length = max(0.0001, note_end - marker_time)
                        self._apply_decoupled_note_loop(clip, marker_time, note_length, send_updates=True)
                    else:
                        clip.loop_start = marker_time
                else:
                    decoupled_info = self._decoupled_automation_info(clip)
                    if decoupled_info:
                        note_length = max(0.0001, marker_time - decoupled_info["note_start"])
                        self._apply_decoupled_note_loop(clip, decoupled_info["note_start"], note_length, send_updates=True)
                    else:
                        clip.loop_end = marker_time
        
        # visible channel and mixer status true
        if len(message) >= 2 and message[1] == 18:
            start = message[2]
            end = message[3]
            self.visible_channels = (start, end)
            self.mixer_status = True
            self._set_up_mixer_controls()
            
        # combine clips
        if len(message) >= 2 and message[1] == 19:
            values = self.extract_values_from_sysex_message(message)
            if len(values) == 4:
                self._append_and_remove_clip(values[0], values[1], values[2], values[3])
        
        # toggle arm for audio tracks
        if len(message) >= 2 and message[1] == 20:
            track_index = message[2]
            track = self.song().tracks[track_index]
            track.arm = not track.arm
        
        # select next clip
        if len(message) >= 2 and message[1] == 21:
            upValue = message[2]
            track = self.song().view.selected_track
            current_clip_slot = self.song().view.highlighted_clip_slot
            # Find current index
            current_index = list(track.clip_slots).index(current_clip_slot)
            
            if upValue == 0:  # Move down to next clip
                # Search for next clip slot with a clip
                for i in range(current_index + 1, len(track.clip_slots)):
                    if track.clip_slots[i].has_clip:
                        self.song().view.highlighted_clip_slot = track.clip_slots[i]
                        break
                        
            elif upValue == 1:  # Move up to previous clip
                # Search for previous clip slot with a clip (in reverse)
                for i in range(current_index - 1, -1, -1):
                    if track.clip_slots[i].has_clip:
                        self.song().view.highlighted_clip_slot = track.clip_slots[i]
                        break
        if len(message) >= 2 and message[1] == 22:
            tempo_bytes = message[2:-1]
            try:
                tempo_string = bytes(tempo_bytes).decode('ascii')
                new_tempo = float(tempo_string)
                self.song().tempo = new_tempo
                # Optional: print for debugging
                # self.canonical_parent.log_message("Tempo set to " + tempo_string)
            except Exception as e:
                # Optional: log error
                # self.canonical_parent.log_message("Tempo decode error: " + str(e))
                pass
        if len(message) >= 3 and message[1] == 23:
            try:
                self.song().metronome = bool(message[2])
            except Exception:
                pass
        if len(message) >= 2 and message[1] == 43:
            self._handle_tap_tempo()
        if len(message) >= 2 and message[1] == 35:
            self._set_follow_action_rule(message)
        if len(message) >= 2 and message[1] == 36:
            self._delete_follow_action_rule(message)
        if len(message) >= 2 and message[1] == 37:
            self._send_follow_action_state(force=True)
        if len(message) >= 2 and message[1] == 38:
            values = self.extract_values_from_sysex_message(message)
            if len(values) == 1:
                self._stop_track_clips(values[0])
        if len(message) >= 2 and message[1] == 39:
            self._set_device_control_high_resolution(message)
        if len(message) >= 3 and message[1] == 44:
            self._toggle_group_fold(message[2])
        if len(message) >= 3 and message[1] == 45:
            self._add_random_effect_after_device(message[2])
        if len(message) >= 3 and message[1] == 46:
            self._set_browser_insert_after_device(message[2])
        if len(message) >= 4 and message[1] == 47:
            self._move_device_after_index(message[2], message[3])
        if len(message) >= 4 and message[1] == 48:
            self._handle_rack_snapshot_command(message)
        if len(message) >= 2 and message[1] == 49:
            self._send_automation_envelope(message)
        if len(message) >= 2 and message[1] == 50:
            self._set_automation_envelope(message)
        if len(message) >= 2 and message[1] == 51:
            self._set_decoupled_automation_length(message)
        if len(message) >= 2 and message[1] == 52:
            self._unfold_decoupled_automation_clip()
        if len(message) >= 2 and message[1] == 53:
            self._clear_automation_envelope(message)
        if len(message) >= 2 and message[1] == 54:
            self._clear_all_automation_envelopes(message)
        if len(message) >= 2 and message[1] == 55:
            self._set_mutator_clip(message)
        if len(message) >= 2 and message[1] == 56:
            action = message[2] if len(message) >= 3 else 0
            if action == 1:
                self._unfold_mutator_clip()
            else:
                self._end_mutator_clip()
        if len(message) >= 2 and message[1] == 57:
            self._update_mutator_clip_settings(message)
        if len(message) >= 2 and message[1] == 58:
            self._replace_rhythm_generator_lane(message)
    def _replace_rhythm_generator_lane(self, message):
        try:
            payload = bytes(message[2:-1]).decode('ascii', errors='ignore')
            fields = payload.split("|")
            if len(fields) < 3:
                return

            pitch = max(0, min(127, int(fields[0])))
            replace_start = max(0.0, int(fields[1]) / 1000.0)
            replace_end = max(replace_start, int(fields[2]) / 1000.0)
            replace_length = max(0.0001, replace_end - replace_start)
            note_payload = fields[3] if len(fields) > 3 else ""

            clip_slot = self.song().view.highlighted_clip_slot
            if clip_slot is None or not clip_slot.has_clip:
                return
            clip = clip_slot.clip
            if not getattr(clip, "is_midi_clip", False):
                return

            if hasattr(clip, "remove_notes_extended"):
                clip.remove_notes_extended(pitch, 1, replace_start, replace_length)

            specs = []
            for item in note_payload.split(";"):
                if not item:
                    continue
                parts = item.split(",")
                if len(parts) < 3:
                    continue
                start_time = max(0.0, int(parts[0]) / 1000.0)
                duration = max(0.0001, int(parts[1]) / 1000.0)
                velocity = max(1, min(127, int(parts[2])))
                specs.append(MidiNoteSpecification(
                    pitch=pitch,
                    start_time=start_time,
                    duration=duration,
                    velocity=velocity,
                    mute=False,
                    probability=1.0
                ))

            if specs:
                clip.add_new_notes(tuple(specs))
            self.send_selected_clip_metadata()
            self.send_selected_clip_notes()
        except Exception as e:
            self._debug_log("Error replacing rhythm generator lane: {}".format(str(e)))

    def _mutator_settings_from_message(self, message):
        payload = bytes(message[2:-1]).decode('ascii', errors='ignore')
        fields = self._split_escaped_sysex_fields(payload, "|")
        def int_field(index, fallback):
            try:
                return int(fields[index])
            except Exception:
                return fallback
        def float_field(index, fallback):
            try:
                return float(fields[index])
            except Exception:
                return fallback
        seed_index = 11
        algorithm_index = 12
        companion_mode_index = 13
        target_pitches_index = 14
        operation_depths_index = 15
        operation_order_index = operation_depths_index + len(self._mutator_operation_depth_keys())
        mutator_slots_index = operation_order_index + 1
        mutator_slot_count_index = mutator_slots_index + 1
        companion_mode = "rhythm" if int_field(companion_mode_index, 0) == 1 else "melody"
        target_pitches = self._mutator_target_pitches_from_value(fields[target_pitches_index] if len(fields) > target_pitches_index else "")
        operation_depths = self._mutator_operation_depths_from_fields(
            fields,
            operation_depths_index,
            self._mutator_default_operation_depths()
        )
        operation_order = self._mutator_operation_order_from_value(
            fields[operation_order_index] if len(fields) > operation_order_index else ""
        )
        mutator_slots = self._mutator_slots_from_value(
            fields[mutator_slots_index] if len(fields) > mutator_slots_index else ""
        )
        mutator_slot_count = self._mutator_slot_count_from_value(
            fields[mutator_slot_count_index] if len(fields) > mutator_slot_count_index else "",
            mutator_slots
        )
        message_scale = self._unescape_sysex_string(fields[9]) if len(fields) > 9 else "Minor"
        message_scale_index = self._mutator_scale_index_from_name(message_scale)
        if message_scale_index is None:
            message_scale_index = 2
        live_scale_index, live_root = self._current_mutator_scale_root(
            message_scale_index,
            max(0, min(11, int_field(10, 0)))
        )
        return {
            "preset": int_field(0, 9),
            "mutations_per_pass": max(1, min(3, int_field(1, 1))),
            "return_after_passes": max(1, min(4, int_field(2, 2))),
            "return_mode": int_field(3, 0),
            "original_loops": max(0, int_field(4, 4)),
            "loops_per_pass": max(1, int_field(5, 1)),
            "regenerate_mode": int_field(6, 0),
            "source_mode": int_field(7, 2),
            "depth": max(0.0, min(1.0, float_field(8, 0.0))),
            "scale": self._mutator_scale_name_from_index(live_scale_index),
            "root": live_root,
            "seed": int_field(seed_index, random.randint(1, 2000000000)),
            "algorithm": self._unescape_sysex_string(fields[algorithm_index]) if len(fields) > algorithm_index else "mutator",
            "companion_mode": companion_mode,
            "target_pitches": target_pitches,
            "fill_depth": operation_depths.get("fill_depth", 0.0),
            "simplification_depth": operation_depths.get("simplification_depth", 0.0),
            "octave_shift_depth": operation_depths.get("octave_shift_depth", 0.0),
            "rhythmic_shift_depth": operation_depths.get("rhythmic_shift_depth", 0.0),
            "note_addition_depth": operation_depths.get("note_addition_depth", 0.0),
            "note_removal_depth": operation_depths.get("note_removal_depth", 0.0),
            "pitch_shift_depth": operation_depths.get("pitch_shift_depth", 0.0),
            "velocity_change_depth": operation_depths.get("velocity_change_depth", 0.0),
            "gate_change_depth": operation_depths.get("gate_change_depth", 0.0),
            "shift_depth": operation_depths.get("shift_depth", 0.0),
            "operation_order": operation_order,
            "mutator_slots": mutator_slots,
            "mutator_slot_count": mutator_slot_count,
            "settings_payload": payload.replace("|", "~")[:80],
        }

    def _mutator_info_from_settings(self, settings, previous_info, clip, commit_structure=False):
        settings_scale_index = self._mutator_scale_index_from_name(settings.get("scale", "Minor"))
        if settings_scale_index is None:
            settings_scale_index = previous_info.get("scale_index", 2)
        scale_index, root = self._current_mutator_scale_root(
            settings_scale_index,
            settings.get("root", previous_info.get("root", 0))
        )
        preset = settings.get("preset", previous_info.get("preset", 9))
        mutator_slots = self._mutator_visible_slots(settings)
        mutator_slot_count = self._mutator_slot_count_from_value(settings.get("mutator_slot_count", ""), self._mutator_normalized_slots(settings))
        info = {
            "original_loop_length": previous_info.get("original_loop_length", 0.0001),
            "structure_length": previous_info.get("structure_length", previous_info.get("original_loop_length", 0.0001)),
            "preset": preset if commit_structure else previous_info.get("preset", preset),
            "settings_preset": preset,
            "seed": settings.get("seed", previous_info.get("seed", 1)) if commit_structure else previous_info.get("seed", settings.get("seed", 1)),
            "algorithm": settings.get("algorithm", previous_info.get("algorithm", "mutator")),
            "algorithm_code": self._mutator_algorithm_code(settings.get("algorithm", previous_info.get("algorithm", "mutator"))),
            "mutations_per_pass": settings.get("mutations_per_pass", previous_info.get("mutations_per_pass", 1)),
            "depth": settings.get("depth", previous_info.get("depth", 0.0)),
            "regenerate_mode": settings.get("regenerate_mode", previous_info.get("regenerate_mode", 0)),
            "source_mode": settings.get("source_mode", previous_info.get("source_mode", 2)),
            "root": root,
            "scale_index": scale_index,
            "companion_mode": settings.get("companion_mode", previous_info.get("companion_mode", "melody")),
            "companion_mode_code": self._mutator_companion_mode_code(settings.get("companion_mode", previous_info.get("companion_mode", "melody"))),
            "target_pitches": settings.get("target_pitches", previous_info.get("target_pitches", [])),
            "operation_order": [int(slot.get("operation", 0)) for slot in self._mutator_active_slots(settings)],
            "mutator_slots": mutator_slots,
            "mutator_slot_count": mutator_slot_count,
            "sections": previous_info.get("sections", []),
        }
        for key in self._mutator_operation_depth_keys():
            info[key] = self._mutator_depth_value(settings, key, previous_info.get(key, 0.0))
        if commit_structure:
            info["pending_settings_update"] = False
        else:
            info["pending_settings_update"] = (
                bool(previous_info.get("pending_settings_update", False)) or
                self._mutator_generation_settings_changed(info, previous_info)
            )
        return info

    def _mutator_generation_settings_changed(self, info, previous_info):
        comparisons = (
            ("settings_preset", int(info.get("settings_preset", info.get("preset", 9))), int(previous_info.get("settings_preset", previous_info.get("preset", 9)))),
            ("algorithm_code", int(info.get("algorithm_code", 0)), int(previous_info.get("algorithm_code", self._mutator_algorithm_code(previous_info.get("algorithm", "mutator"))))),
            ("mutations_per_pass", int(info.get("mutations_per_pass", 1)), int(previous_info.get("mutations_per_pass", 1))),
            ("source_mode", int(info.get("source_mode", 2)), int(previous_info.get("source_mode", 2))),
            ("root", int(info.get("root", 0)), int(previous_info.get("root", 0))),
            ("scale_index", int(info.get("scale_index", 2)), int(previous_info.get("scale_index", 2))),
            ("companion_mode_code", int(info.get("companion_mode_code", 0)), int(previous_info.get("companion_mode_code", self._mutator_companion_mode_code(previous_info.get("companion_mode", "melody"))))),
        )
        for _, current, previous in comparisons:
            if current != previous:
                return True
        if tuple(info.get("target_pitches", [])) != tuple(previous_info.get("target_pitches", [])):
            return True
        if tuple(info.get("operation_order", [])) != tuple(previous_info.get("operation_order", [])):
            return True
        if tuple(str(slot) for slot in self._mutator_visible_slots(info)) != tuple(str(slot) for slot in self._mutator_visible_slots(previous_info)):
            return True
        if int(info.get("mutator_slot_count", 4)) != int(previous_info.get("mutator_slot_count", 4)):
            return True
        for key in self._mutator_operation_depth_keys():
            try:
                if abs(self._mutator_depth_value(info, key, 0.0) - self._mutator_depth_value(previous_info, key, 0.0)) > 0.005:
                    return True
            except Exception:
                pass
        try:
            return abs(float(info.get("depth", 0.0)) - float(previous_info.get("depth", 0.0))) > 0.005
        except Exception:
            return False

    def _mutator_pattern_roles(self, preset):
        patterns = {
            0: [0, 0, 0, 0, 5, 5, 5, 5],
            1: [0, 0, 0, 0, 4, 4, 5, 5],
            2: [0, 5, 6, 9, 6, 12, 5, 0],
            3: [0, 0, 1, 1, 5, 7, 1, 8],
            4: [0, 0, 1, 1, 5, 7, 6, 6, 1, 8],
            5: [0, 0, 0, 1, 1, 5, 7, 6, 6, 5, 7, 6, 6, 1, 8],
            6: [0, 5, 6, 9],
            7: [0, 0, 5, 5, 6, 6, 9, 9],
            8: [0, 0, 0, 0, 5, 6, 9, 10],
            9: [0, 5],
            10: [0, 0, 5, 5],
            11: [0, 0, 5, 8],
            12: [0, 5, 8, 5, 6, 5],
            13: [0, 1, 2, 5, 6, 8],
            14: [0, 1, 2, 7, 5, 13, 14, 6, 15, 8],
            15: [0, 0, 5, 6, 9, 10],
            16: [0, 0, 5, 5, 16, 16],
            17: [0, 0, 5, 16],
            18: [0, 0, 5, 5, 0, 0, 16, 16],
        }
        return patterns.get(int(preset), patterns[9])

    def _mutator_preset_is_chain(self, preset):
        return int(preset) in (2, 6, 7, 8, 15)

    def _mutator_role_depth(self, role, global_depth):
        ranges = {
            0: (0.0, 0.10),
            1: (0.08, 0.22),
            2: (0.10, 0.26),
            3: (0.50, 0.68),
            4: (0.25, 0.60),
            5: (0.35, 0.70),
            6: (0.20, 0.55),
            7: (0.45, 0.75),
            8: (0.0, 0.0),
            9: (0.30, 0.60),
            10: (0.35, 0.65),
            11: (0.40, 0.70),
            12: (0.38, 0.68),
            13: (0.42, 0.72),
            14: (0.20, 0.50),
            15: (0.45, 0.78),
            16: (0.35, 0.70),
        }
        low, high = ranges.get(role, (0.15, 0.45))
        return low + ((high - low) * max(0.0, min(1.0, global_depth)))

    def _mutator_algorithm_kind(self, algorithm):
        if algorithm in ("verse_weaver", "motif_ladder"):
            return "expand"
        if algorithm in ("sparse_echo",):
            return "variations"
        if algorithm in ("chorus_lift", "middle_eight", "tension_break"):
            return "section"
        if algorithm in ("skylight_hook", "glass_steps", "nocturne_line"):
            return "melody"
        if algorithm in ("modal_drift", "circle_resolve"):
            return "chords"
        return None

    def _mutator_scale_notes(self, settings):
        intervals = self._mutator_scale_intervals(settings.get("scale", "Minor"))
        root = int(settings.get("root", 0)) % 12
        notes = [pitch for pitch in range(128) if ((pitch - root) % 12) in intervals]
        return notes if notes else list(range(128))

    def _mutator_scale_index(self, pitch, scale_notes):
        return min(range(len(scale_notes)), key=lambda index: abs(scale_notes[index] - int(round(pitch))))

    def _mutator_transpose_scale_steps(self, pitch, steps, settings):
        if settings.get("scale", "Minor") == "Chromatic":
            return max(0, min(127, int(round(pitch + steps))))
        scale_notes = self._mutator_scale_notes(settings)
        index = self._mutator_scale_index(pitch, scale_notes)
        return scale_notes[max(0, min(len(scale_notes) - 1, index + int(steps)))]

    def _mutator_scale_step_span_for_octave_fraction(self, pitch, amount, settings):
        amount = max(0.0, min(1.0, float(amount)))
        if amount <= 0.0:
            return 0
        if settings.get("scale", "Minor") == "Chromatic":
            return max(1, int(math.ceil(amount * 12.0)))

        scale_notes = self._mutator_scale_notes(settings)
        index = self._mutator_scale_index(pitch, scale_notes)
        source_pitch = scale_notes[index]
        semitone_span = max(1, int(math.ceil(amount * 12.0)))
        upper = index
        while upper + 1 < len(scale_notes) and scale_notes[upper + 1] - source_pitch <= semitone_span:
            upper += 1
        lower = index
        while lower - 1 >= 0 and source_pitch - scale_notes[lower - 1] <= semitone_span:
            lower -= 1
        return max(1, upper - index, index - lower)

    def _mutator_degree_pitch(self, degree, octave, settings):
        intervals = self._mutator_scale_intervals(settings.get("scale", "Minor"))
        degree = int(degree)
        octave_shift = degree // len(intervals)
        degree_index = degree % len(intervals)
        pitch = 12 * (octave + 1 + octave_shift) + (int(settings.get("root", 0)) % 12) + intervals[degree_index]
        return self._mutator_quantize_pitch(pitch, settings)

    def _mutator_algorithm_source_steps(self, kind, algorithm, bar, total_bars, cycle):
        expand_patterns = {
            "verse_weaver": [0, 0, 1, 1, -1, -1, 2, 0],
            "motif_ladder": [0, 1, 2, 3, 2, 1, 0, -1],
        }
        variation_patterns = {
            "sparse_echo": [0, -2, 0, 2],
        }
        section_patterns = {
            "chorus_lift": [2, 2, 3, 3, 4, 4, 2, 0],
            "middle_eight": [-2, 1, -1, 2, -3, 2, 1, -1],
            "tension_break": [4, 4, 3, 4, 5, 4, 3, 4],
        }
        if kind == "expand":
            pattern = expand_patterns.get(algorithm, expand_patterns["verse_weaver"])
        elif kind == "variations":
            pattern = variation_patterns.get(algorithm, variation_patterns["sparse_echo"])
        else:
            pattern = section_patterns.get(algorithm, section_patterns["chorus_lift"])
        return pattern[(bar + cycle) % len(pattern)]

    def _mutator_algorithm_prune_values(self, values, loop_length):
        cleaned = []
        for value in values:
            start = max(0.0, float(value.get("start", 0.0)))
            if start >= loop_length:
                continue
            duration = max(0.03125, min(float(value.get("duration", 0.03125)), loop_length - start))
            cleaned.append(dict(
                value,
                pitch=max(0, min(127, int(round(value.get("pitch", 60))))),
                start=start,
                duration=duration,
                velocity=max(1, min(127, int(round(value.get("velocity", 96))))),
                mute=bool(value.get("mute", False)),
                probability=max(0.0, min(1.0, float(value.get("probability", 1.0)))),
            ))
        cleaned.sort(key=lambda item: (item["start"], item["pitch"], item["duration"]))
        return cleaned

    def _mutator_make_algorithm_section_values(self, source_values, role, source_start, loop_length, settings, rnd):
        algorithm = settings.get("algorithm", "mutator")
        kind = self._mutator_algorithm_kind(algorithm)
        if not kind:
            return None

        try:
            bar_beats = float(max(1, int(self.song().signature_numerator)))
        except Exception:
            bar_beats = 4.0
        bar_beats = min(max(1.0, bar_beats), max(1.0, float(loop_length)))
        total_bars = max(1, int(math.ceil(float(loop_length) / bar_beats)))

        source = []
        for value in tuple(source_values or ()):
            relative_start = max(0.0, min(loop_length - 0.0001, float(value.get("start", 0.0)) - float(source_start)))
            source.append(dict(value, start=relative_start, duration=min(float(value.get("duration", 0.03125)), loop_length - relative_start)))
        if not source:
            return None

        chord_starts = {}
        for value in source:
            key = round(value["start"], 3)
            chord_starts[key] = chord_starts.get(key, 0) + 1
        result = []
        variation = max(0.0, min(1.0, float(settings.get("depth", 0.0))))

        def random_step_variation(spread=2, chance=None, upward_bias=0):
            if chance is None:
                chance = 0.18 + (variation * 0.42)
            if rnd.random() >= chance:
                return 0
            choices = []
            for amount in range(1, max(1, int(spread)) + 1):
                choices.extend([-amount, amount])
                if upward_bias > 0:
                    choices.extend([amount] * int(upward_bias))
            return rnd.choice(choices or [0])

        melody_offsets = {
            "skylight_hook": [0, 1, 2, 3, 2, 1, 0, -1],
            "glass_steps": [0, 1, 1, 2, -1, 2, 1, 0],
            "nocturne_line": [0, -1, -2, -1, 0, 1, -1, 0],
        }
        chord_progressions = {
            "modal_drift": [0, -1, 2, 1, 3, 1, 2, 4],
            "circle_resolve": [-2, 1, -1, 0, 2, -1, 1, 4],
        }
        role_weight = {
            1: 1, 2: 2, 3: 3, 4: 2, 5: 3, 6: 2, 7: 2, 9: 3, 10: 4, 11: 5, 12: 4
        }.get(role, 1)

        for index, value in enumerate(source):
            is_chord_note = chord_starts.get(round(value["start"], 3), 0) > 1
            start = value["start"]
            bar = int(start / bar_beats)
            new_value = dict(value)

            if kind == "melody":
                offsets = melody_offsets.get(algorithm, melody_offsets["skylight_hook"])
                steps = offsets[(index + role) % len(offsets)] + random_step_variation(spread=2, upward_bias=1 if algorithm in ("skylight_hook", "glass_steps") else 0)
                if algorithm == "glass_steps":
                    steps += 1 if role_weight >= 3 and index % 3 == 1 else 0
                    if role_weight >= 4 and rnd.random() < 0.35 + (variation * 0.35):
                        steps += 1
                elif algorithm == "nocturne_line":
                    steps = max(-3, min(2, steps))
                elif algorithm == "skylight_hook" and role_weight >= 3 and rnd.random() < 0.25 + (variation * 0.35):
                    steps += 1
                new_value["pitch"] = self._mutator_transpose_scale_steps(value["pitch"], steps, settings)
                new_value["duration"] = value["duration"]
                velocity_lift = (4 if algorithm == "glass_steps" else 2) + rnd.randint(0, int(2 + (variation * 6)))
                new_value["velocity"] = max(1, min(127, int(round(value["velocity"] + velocity_lift))))

            elif kind == "chords":
                progression = chord_progressions.get(algorithm, chord_progressions["modal_drift"])
                steps = progression[(bar + role) % len(progression)] + random_step_variation(spread=2, chance=0.22 + (variation * 0.45))
                if role in (10, 11) or (role_weight >= 4 and bar >= total_bars - 1):
                    steps += 3
                root_pitch = self._mutator_transpose_scale_steps(value["pitch"], steps, settings)
                chord_velocity = max(1, min(127, int(round(value["velocity"] + 2 + rnd.randint(0, int(2 + variation * 6))))))
                if is_chord_note:
                    result.append(dict(value, pitch=root_pitch, start=start, duration=value["duration"], velocity=chord_velocity))
                    continue
                root_value = dict(value, pitch=root_pitch, start=start, duration=value["duration"], velocity=chord_velocity)
                third = dict(root_value, pitch=self._mutator_transpose_scale_steps(root_pitch, 2, settings), velocity=max(1, min(127, root_value["velocity"] - 8)))
                fifth_steps = 5 if role in (10, 11) else 4
                fifth = dict(root_value, pitch=self._mutator_transpose_scale_steps(root_pitch, fifth_steps, settings), velocity=max(1, min(127, root_value["velocity"] - 12)))
                result.extend([root_value, third, fifth])
                if algorithm == "circle_resolve" and role_weight >= 3 and (bar + role) % 2 == 0:
                    result.append(dict(root_value, pitch=self._mutator_transpose_scale_steps(root_pitch, 6, settings), velocity=max(1, min(127, root_value["velocity"] - 16))))
                continue

            else:
                steps = self._mutator_algorithm_source_steps(kind, algorithm, bar, total_bars, role)
                if algorithm == "motif_ladder":
                    steps += random_step_variation(spread=2, chance=0.20 + (variation * 0.45), upward_bias=1)
                elif algorithm in ("middle_eight", "tension_break"):
                    steps += random_step_variation(spread=2, chance=0.25 + (variation * 0.45), upward_bias=1 if algorithm == "tension_break" else 0)
                elif algorithm == "sparse_echo":
                    steps += random_step_variation(spread=1, chance=0.12 + (variation * 0.25))
                new_value["pitch"] = self._mutator_transpose_scale_steps(new_value["pitch"], steps, settings)

            if kind == "section" and algorithm == "middle_eight" and bar % 2 == 1:
                start += rnd.choice([0.0, 0.125, 0.25]) if variation > 0.25 else 0.125
            if kind == "section" and algorithm == "middle_eight":
                new_value["duration"] = value["duration"] * (1.35 if is_chord_note else 1.15)
            elif kind == "section" and algorithm == "tension_break":
                new_value["duration"] = max(0.0625, value["duration"] * rnd.choice([0.75, 0.9, 1.0]))
            lift = 0
            if kind == "section" and algorithm == "chorus_lift":
                lift = 8 + role_weight
            elif kind == "section" and algorithm == "tension_break":
                lift = 4 + rnd.randint(0, int(4 + variation * 8))
            elif kind == "variations" and algorithm == "sparse_echo":
                lift = rnd.randint(8, int(14 + variation * 10))
            elif kind == "expand":
                lift = 4 + min(4, role_weight) + rnd.randint(0, int(2 + variation * 5))
            new_value["start"] = start
            new_value["velocity"] = max(1, min(127, int(round(value["velocity"] + lift))))
            result.append(new_value)

            if not is_chord_note and algorithm == "sparse_echo" and rnd.random() < 0.46:
                echo_velocity = new_value["velocity"] if rnd.random() < 0.35 else max(1, int(round(new_value["velocity"] * rnd.uniform(0.78, 0.98))))
                result.append(dict(new_value, start=start + min(0.5, max(0.125, value["duration"])), duration=max(0.0625, value["duration"] * 0.55), pitch=self._mutator_transpose_scale_steps(new_value["pitch"], 5 + random_step_variation(spread=1, chance=0.25 + variation * 0.25), settings), velocity=echo_velocity, probability=min(new_value.get("probability", 1.0), rnd.uniform(0.82, 1.0))))
            elif kind == "section" and algorithm == "chorus_lift" and not is_chord_note and rnd.random() < 0.18:
                result.append(dict(new_value, pitch=min(127, new_value["pitch"] + 12), velocity=max(1, new_value["velocity"] - 20)))

        return self._mutator_algorithm_prune_values(result, loop_length)

    def _mutator_scale_intervals(self, scale_name):
        table = {
            "Chromatic": list(range(12)),
            "Major": [0, 2, 4, 5, 7, 9, 11],
            "Minor": [0, 2, 3, 5, 7, 8, 10],
            "Dorian": [0, 2, 3, 5, 7, 9, 10],
            "Mixolydian": [0, 2, 4, 5, 7, 9, 10],
            "Lydian": [0, 2, 4, 6, 7, 9, 11],
            "Phrygian": [0, 1, 3, 5, 7, 8, 10],
            "Locrian": [0, 1, 3, 5, 6, 8, 10],
            "Minor Pentatonic": [0, 3, 5, 7, 10],
            "Major Pentatonic": [0, 2, 4, 7, 9],
            "Minor Blues": [0, 3, 5, 6, 7, 10],
        }
        return table.get(scale_name, table["Minor"])

    def _mutator_quantize_pitch(self, pitch, settings):
        intervals = self._mutator_scale_intervals(settings.get("scale", "Minor"))
        root = int(settings.get("root", 0))
        best = int(pitch)
        best_distance = 128
        for candidate in range(max(0, int(pitch) - 12), min(127, int(pitch) + 12) + 1):
            rel = (candidate - root) % 12
            if rel in intervals:
                distance = abs(candidate - int(pitch))
                if distance < best_distance:
                    best = candidate
                    best_distance = distance
        return max(0, min(127, best))

    def _mutator_note_values(self, note):
        return {
            "pitch": int(note.pitch),
            "start": float(note.start_time),
            "duration": max(0.0001, float(note.duration)),
            "velocity": int(note.velocity),
            "mute": bool(note.mute),
            "probability": float(getattr(note, "probability", 1.0)),
        }

    def _mutator_specs_from_values(self, values):
        return MidiNoteSpecification(
            pitch=max(0, min(127, int(values["pitch"]))),
            start_time=max(0.0, float(values["start"])),
            duration=max(0.0001, float(values["duration"])),
            velocity=max(1, min(127, int(values["velocity"]))),
            mute=bool(values.get("mute", False)),
            probability=max(0.0, min(1.0, float(values.get("probability", 1.0))))
        )

    def _mutator_place_section_values(self, values, section_start, loop_length):
        specs = []
        for value in tuple(values or ()):
            relative_start = max(0.0, min(float(loop_length) - 0.0001, float(value.get("start", 0.0))))
            duration = max(0.0001, min(float(value.get("duration", 0.0001)), float(loop_length) - relative_start))
            specs.append(self._mutator_specs_from_values(dict(
                value,
                start=float(section_start) + relative_start,
                duration=duration
            )))
        return specs

    def _mutator_relative_section_values(self, source_values, source_start, loop_length):
        result = []
        for value in tuple(source_values or ()):
            relative_start = max(0.0, min(float(loop_length) - 0.0001, float(value.get("start", 0.0)) - float(source_start)))
            result.append(dict(
                value,
                start=relative_start,
                duration=min(float(value.get("duration", 0.0001)), float(loop_length) - relative_start)
            ))
        return result

    def _mutator_rhythm_target_pitches(self, settings, source_values):
        targets = [int(pitch) for pitch in settings.get("target_pitches", []) if 0 <= int(pitch) <= 127]
        return targets[:16]

    def _mutator_split_rhythm_source_values(self, settings, source_values, source_start, loop_length):
        target_pitches = set(self._mutator_rhythm_target_pitches(settings, source_values))
        selected_values = []
        passthrough_values = []
        for value in tuple(source_values or ()):
            pitch = int(value.get("pitch", 0))
            if pitch in target_pitches:
                selected_values.append(value)
            else:
                passthrough_values.append(value)
        passthrough_section_values = self._mutator_relative_section_values(
            passthrough_values,
            source_start,
            loop_length
        )
        return selected_values, passthrough_section_values

    def _mutator_rhythm_occupied_steps(self, values, loop_length=None, grid=0.125):
        occupied = set()
        step_count = max(1, int(round(float(loop_length) / float(grid)))) if loop_length else None
        for value in tuple(values or ()):
            try:
                pitch = int(value.get("pitch", 0))
                step = int(round(float(value.get("start", 0.0)) / float(grid)))
                if step_count:
                    step = step % step_count
                occupied.add((pitch, step))
            except Exception:
                pass
        return occupied

    def _mutator_rhythm_free_start(self, pitch, start, loop_length, occupied, grid=0.125):
        steps = max(1, int(round(float(loop_length) / float(grid))))
        step = int(round(float(start) / float(grid))) % steps
        for offset in range(steps):
            candidate_step = (step + offset) % steps
            key = (int(pitch), candidate_step)
            if key not in occupied:
                occupied.add(key)
                return min(float(loop_length) - 0.0001, candidate_step * float(grid))
        return None

    def _mutator_velocity_delta(self, rnd, depth, role, rhythm=False):
        amount = max(0.0, min(1.0, float(depth)))
        if amount <= 0.0:
            return 0
        spread = max(1, int(round(amount * (48 if rhythm else 34))))
        return rnd.randint(-spread, spread)

    def _mutator_rhythm_gate_duration(self, duration, remaining, depth, rnd):
        amount = max(0.0, min(1.0, float(depth)))
        current = max(0.0001, float(duration))
        limit = max(0.0001, float(remaining))
        if current <= 0.125:
            short_targets = [0.03125, 0.0625, 0.09375, 0.125, 0.1875, 0.25, 0.3125, 0.375, 0.5]
            target_limit = min(limit, max(current * (1.0 + amount * 3.0), 0.0625 + amount * 0.3125))
            candidates = [
                value
                for value in short_targets
                if value <= target_limit + 0.000001 and abs(value - current) >= 0.015625
            ]
            if not candidates:
                candidates = [max(0.03125, min(target_limit, current * (0.5 if current > 0.03125 else 2.0)))]
            return max(0.03125, min(limit, rnd.choice(candidates)))
        multiplier_choices = [0.35, 0.5, 0.75, 1.25, 1.5, 2.0]
        if amount >= 0.75:
            multiplier_choices += [0.25, 2.5]
        return max(0.03125, min(limit, current * rnd.choice(multiplier_choices)))

    def _mutator_gate_duration(self, duration, remaining, strength, rnd):
        amount = max(0.0, min(1.0, float(strength)))
        current = max(0.0001, float(duration))
        limit = max(0.0001, float(remaining))
        if amount <= 0.0:
            return min(limit, current)
        if rnd.random() < 0.5:
            if amount <= 0.5:
                span = 0.125 + amount * 0.75
                multiplier = rnd.uniform(max(0.1, 1.0 - span), 1.0)
            else:
                low = 0.5 - ((amount - 0.5) / 0.5) * 0.4
                multiplier = rnd.uniform(max(0.1, low), 1.0)
        else:
            if amount <= 0.5:
                span = 0.125 + amount * 0.75
                multiplier = rnd.uniform(1.0, 1.0 + span)
            elif rnd.random() < 0.78:
                multiplier = rnd.uniform(1.75, 2.5 + amount * 0.75)
            else:
                high = 1.5 + ((amount - 0.5) / 0.5) * 8.5
                multiplier = rnd.uniform(1.0, high)
        return max(0.03125, min(limit, current * multiplier))

    def _mutator_operation_count(self, count, depth, rnd=None):
        amount = max(0.0, min(1.0, float(depth)))
        if count <= 0 or amount <= 0.0:
            return 0
        if amount >= 1.0:
            return int(count)
        if rnd is None:
            return max(0, min(int(count), int(round(float(count) * amount))))
        return sum(1 for _ in range(int(count)) if rnd.random() < amount)

    def _mutator_operation_indexes(self, count, depth, rnd):
        amount = max(0.0, min(1.0, float(depth)))
        if count <= 0 or amount <= 0.0:
            return []
        if amount >= 1.0:
            return list(range(int(count)))
        return [index for index in range(int(count)) if rnd.random() < amount]

    def _mutator_deterministic_count(self, count, amount, minimum_when_positive=True):
        amount = max(0.0, min(1.0, float(amount)))
        count = max(0, int(count))
        if count <= 0 or amount <= 0.0:
            return 0
        result = max(0, min(count, int(round(float(count) * amount))))
        if minimum_when_positive:
            result = max(1, result)
        return result

    def _mutator_deterministic_indexes(self, count, amount, rnd, minimum_when_positive=True):
        target_count = self._mutator_deterministic_count(count, amount, minimum_when_positive=minimum_when_positive)
        if target_count <= 0:
            return []
        indexes = list(range(int(count)))
        rnd.shuffle(indexes)
        return sorted(indexes[:target_count])

    def _mutator_has_sub_sixteenth_notes(self, values):
        for value in tuple(values or ()):
            try:
                start = float(value.get("start", 0.0))
                if abs((start / 0.125) - round(start / 0.125)) <= 0.00001 and abs((start / 0.25) - round(start / 0.25)) > 0.00001:
                    return True
            except Exception:
                pass
        return False

    def _mutator_timing_grid(self, values):
        return 0.125 if self._mutator_has_sub_sixteenth_notes(values) else 0.25

    def _mutator_addition_grid(self, values):
        has_sixteenth = False
        for value in tuple(values or ()):
            try:
                start = float(value.get("start", 0.0))
                if abs((start / 0.125) - round(start / 0.125)) <= 0.00001 and abs((start / 0.25) - round(start / 0.25)) > 0.00001:
                    return 0.125
                if abs((start / 0.25) - round(start / 0.25)) <= 0.00001 and abs((start / 0.5) - round(start / 0.5)) > 0.00001:
                    has_sixteenth = True
            except Exception:
                pass
        return 0.25 if has_sixteenth else 0.5

    def _mutator_quantized_time(self, start, loop_length, grid):
        grid = max(0.0001, float(grid))
        loop_length = max(0.0001, float(loop_length))
        return max(0.0, min(loop_length - 0.0001, round(float(start) / grid) * grid))

    def _mutator_pack_grid(self, values):
        pack = [dict(value) for value in tuple(values or ())]
        if not pack:
            return 0.25
        starts = []
        for value in pack:
            starts.append(max(0.0, float(value.get("start", 0.0))))
        for grid in (1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125):
            tolerance = min(0.026, grid * 0.22)
            if all(abs(start - (round(start / grid) * grid)) <= tolerance for start in starts):
                return grid
        if len(starts) > 2:
            required_matches = len(starts) - 1
            for grid in (1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125):
                tolerance = min(0.026, grid * 0.22)
                matches = sum(1 for start in starts if abs(start - (round(start / grid) * grid)) <= tolerance)
                if matches >= required_matches:
                    return grid
        return 0.03125

    def _mutator_note_start_grid(self, start):
        start = max(0.0, float(start))
        for grid in (1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125):
            tolerance = min(0.026, grid * 0.22)
            if abs(start - (round(start / grid) * grid)) <= tolerance:
                return grid
        return 0.125

    def _mutator_notes_overlap(self, left, right):
        if int(left.get("pitch", 60)) != int(right.get("pitch", 60)):
            return False
        left_start = float(left.get("start", 0.0))
        right_start = float(right.get("start", 0.0))
        left_end = left_start + max(0.0001, float(left.get("duration", 0.0001)))
        right_end = right_start + max(0.0001, float(right.get("duration", 0.0001)))
        return left_start < right_end - 0.000001 and right_start < left_end - 0.000001

    def _mutator_sample_note_traits(self, pool, rnd, fallback=None, minimum_duration=0.03125, maximum_duration=None):
        pool = [dict(value) for value in tuple(pool or ()) if value]
        fallback = dict(fallback or (pool[0] if pool else {}))
        durations = [max(minimum_duration, float(value.get("duration", fallback.get("duration", 0.125)))) for value in pool]
        velocities = [max(1, min(127, int(value.get("velocity", fallback.get("velocity", 96))))) for value in pool]
        mutes = [bool(value.get("mute", fallback.get("mute", False))) for value in pool]
        probabilities = [max(0.0, min(1.0, float(value.get("probability", fallback.get("probability", 1.0))))) for value in pool]
        duration = rnd.choice(durations) if durations else max(minimum_duration, float(fallback.get("duration", 0.125)))
        if maximum_duration is not None:
            duration = min(duration, max(minimum_duration, float(maximum_duration)))
        return {
            "duration": max(minimum_duration, duration),
            "velocity": rnd.choice(velocities) if velocities else max(1, min(127, int(fallback.get("velocity", 96)))),
            "mute": rnd.choice(mutes) if mutes else bool(fallback.get("mute", False)),
            "probability": rnd.choice(probabilities) if probabilities else max(0.0, min(1.0, float(fallback.get("probability", 1.0)))),
        }

    def _mutator_phrase_start_candidates(self, source_values, result_values, base, pitch, loop_length, rnd, grid, prefer_global_open=False):
        loop_length = max(0.0001, float(loop_length))
        grid = max(0.03125, float(grid))
        step_count = max(1, int(round(loop_length / grid)))
        pitch = int(pitch)

        def quantized(start):
            return max(0.0, min(loop_length - 0.0001, round(float(start) / grid) * grid))

        def start_key(start):
            return int(round(quantized(start) * 960.0))

        occupied_starts = set()
        occupied_pitch_starts = set()
        for value in tuple(result_values or ()):
            try:
                value_pitch = int(value.get("pitch", 60))
                key = start_key(float(value.get("start", 0.0)))
                occupied_starts.add(key)
                occupied_pitch_starts.add((value_pitch, key))
            except Exception:
                pass

        source = [
            dict(value)
            for value in tuple(source_values or ())
            if 0.0 <= float(value.get("start", 0.0)) < loop_length
        ]
        if not source:
            source = [dict(base)]
        source.sort(key=lambda item: (float(item.get("start", 0.0)), int(item.get("pitch", 60))))
        same_pitch = [value for value in source if int(value.get("pitch", 60)) == pitch] or source
        base_start = quantized(float(base.get("start", 0.0)))

        def unique_starts(values):
            seen = set()
            starts = []
            for value in values:
                start = quantized(float(value.get("start", 0.0)))
                key = start_key(start)
                if key in seen:
                    continue
                seen.add(key)
                starts.append(start)
            return sorted(starts)

        starts = unique_starts(source)
        pitch_starts = unique_starts(same_pitch)

        def intervals_from(starts_to_scan):
            counts = {}
            if len(starts_to_scan) < 2:
                return []
            ordered = sorted(starts_to_scan)
            pairs = list(zip(ordered[:-1], ordered[1:]))
            pairs.append((ordered[-1], ordered[0] + loop_length))
            for left, right in pairs:
                interval = quantized(right - left)
                if interval < grid - 0.000001 or interval > 2.0 + 0.000001:
                    continue
                key = int(round(interval / grid))
                counts[key] = counts.get(key, 0) + 1
            ranked = sorted(counts.items(), key=lambda item: (-item[1], item[0]))
            return [key * grid for key, _ in ranked[:5]]

        intervals = intervals_from(pitch_starts) + intervals_from(starts)
        if not intervals:
            intervals = [grid, grid * 2.0, grid * 3.0, grid * 4.0]

        phrase_last = max(starts) if starts else base_start
        phrase_first = min(starts) if starts else base_start
        pitch_last = max(pitch_starts) if pitch_starts else phrase_last
        pitch_first = min(pitch_starts) if pitch_starts else phrase_first
        offsets = sorted(set(quantized(start % 1.0) for start in pitch_starts), key=lambda offset: abs(offset - (base_start % 1.0)))
        if not offsets:
            offsets = [base_start % 1.0]

        candidates = []

        def add_candidate(start, mode_score, local_score=0.0):
            start = quantized(start % loop_length)
            key = start_key(start)
            score = float(mode_score) + float(local_score)
            if (pitch, key) in occupied_pitch_starts:
                score += 1000.0
            if key in occupied_starts:
                score += 7.5 if prefer_global_open else 1.35
            else:
                score -= 0.55 if prefer_global_open else 0.25
            score += rnd.random() * 0.025
            candidates.append((score, start))

        for order, interval in enumerate(intervals[:4]):
            add_candidate(base_start + interval, 0.0, order * 0.08)
            add_candidate(pitch_last + interval, 0.12, order * 0.08)
            add_candidate(phrase_last + interval, 0.35, order * 0.08)
            add_candidate(base_start - interval, 1.3, order * 0.08)

        for order, interval in enumerate(intervals[:4]):
            pickup = loop_length - interval
            if pitch_first <= interval + grid:
                pickup = max(0.0, loop_length - max(grid, interval - pitch_first))
            add_candidate(pickup, 0.55, order * 0.12)
            add_candidate(loop_length - max(grid, interval * 0.5), 0.75, order * 0.12)

        beat = math.floor(base_start)
        answer_offsets = (0.5, 0.75, 1.0, 1.5, -0.5)
        for order, offset in enumerate(answer_offsets):
            add_candidate(base_start + offset, 0.9, order * 0.06)
            add_candidate(beat + offset, 1.05, order * 0.06)

        for cell_delta in (1, 2, 3, 4, -1, -2, 6, -4, 8, -6):
            cell_start = quantized(base_start + (cell_delta * grid))
            for order, offset in enumerate(offsets[:4]):
                add_candidate(math.floor(cell_start) + offset, 1.6, abs(cell_delta) * 0.02 + order * 0.06)

        for step in range(step_count):
            start = step * grid
            key = start_key(start)
            if key not in occupied_starts:
                distance = min((step - int(round(base_start / grid))) % step_count, (int(round(base_start / grid)) - step) % step_count)
                add_candidate(start, 2.15, distance * 0.015)

        seen = set()
        ordered = []
        for _, start in sorted(candidates, key=lambda item: item[0]):
            key = start_key(start)
            if key in seen:
                continue
            seen.add(key)
            ordered.append(start)
        return ordered

    def _mutator_apply_break_role(self, values, loop_length, rhythm=False):
        values = [dict(value) for value in tuple(values or ())]
        if not values:
            return []
        values.sort(key=lambda item: (float(item.get("start", 0.0)), int(item.get("pitch", 0))))
        keep_stride = 3 if rhythm else 2
        kept = [value for index, value in enumerate(values) if index % keep_stride == 0] or values[:1]
        result = []
        for value in kept:
            start = max(0.0, min(float(loop_length) - 0.0001, float(value.get("start", 0.0))))
            duration = min(float(loop_length) - start, max(float(value.get("duration", 0.125)) * 1.35, 0.125))
            result.append(dict(
                value,
                start=start,
                duration=max(0.03125, duration),
                velocity=max(1, min(127, int(value.get("velocity", 96)) - (18 if rhythm else 14))),
            ))
        return result

    def _mutator_simplify_values(self, values, depth, rnd):
        values = [dict(value) for value in tuple(values or ())]
        remove_count = min(max(0, len(values) - 1), self._mutator_operation_count(len(values), depth, rnd))
        if remove_count <= 0:
            return values
        ranked = sorted(
            range(len(values)),
            key=lambda index: (
                int(values[index].get("velocity", 96)),
                float(values[index].get("duration", 0.125)),
                -abs(float(values[index].get("start", 0.0)) - round(float(values[index].get("start", 0.0)))),
            )
        )
        remove_indexes = set(ranked[:remove_count])
        return [value for index, value in enumerate(values) if index not in remove_indexes]

    def _mutator_shift_starts_by_depth(self, values, probability, strength, loop_length, rnd, rhythm=False):
        values = [dict(value) for value in tuple(values or ())]
        indexes = self._mutator_operation_indexes(len(values), probability, rnd)
        if not indexes:
            return values
        grid = self._mutator_timing_grid(values)
        max_steps = max(1, min(4 if rhythm else 3, int(math.ceil(max(0.0, min(1.0, strength)) * (4 if rhythm else 3)))))
        choices = [step * grid for step in range(-max_steps, max_steps + 1) if step != 0]
        for index in indexes:
            start = float(values[index].get("start", 0.0)) + rnd.choice(choices)
            values[index]["start"] = self._mutator_quantized_time(start, loop_length, grid)
        return values

    def _mutator_shift_pitch_groups_by_depth(self, values, probability, strength, loop_length, rnd, rhythm=False, target_pitches=None):
        values = [dict(value) for value in tuple(values or ())]
        amount = max(0.0, min(1.0, float(probability)))
        shift_range = max(0.0, min(1.0, float(strength)))
        loop_length = max(0.0001, float(loop_length))
        if amount <= 0.0 or shift_range <= 0.0 or not values:
            return values

        pitches = sorted(set(int(value.get("pitch", 60)) for value in values if 0 <= int(value.get("pitch", 60)) <= 127))
        if rhythm:
            selected = set(int(pitch) for pitch in tuple(target_pitches or ()) if 0 <= int(pitch) <= 127)
            if not selected:
                return values
            pitches = [pitch for pitch in pitches if pitch in selected]
        if not pitches:
            return values

        grid = self._mutator_timing_grid(values)
        max_steps = max(1, min(8, int(math.ceil((loop_length / grid) * shift_range))))
        offsets_by_pitch = {}
        for pitch in pitches:
            pitch_rnd = random.Random(rnd.randint(0, 2147483647))
            if pitch_rnd.random() > amount:
                continue
            step_choices = [step for step in range(-max_steps, max_steps + 1) if step != 0]
            if step_choices:
                offsets_by_pitch[pitch] = pitch_rnd.choice(step_choices) * grid
        if not offsets_by_pitch:
            return values

        for value in values:
            pitch = int(value.get("pitch", 60))
            if pitch not in offsets_by_pitch:
                continue
            start = (float(value.get("start", 0.0)) + offsets_by_pitch[pitch]) % loop_length
            value["start"] = self._mutator_quantized_time(start, loop_length, grid)
        return values

    def _mutator_loop_shift_by_depth(self, values, probability, loop_length, rnd):
        values = [dict(value) for value in tuple(values or ())]
        amount = max(0.0, min(1.0, float(probability)))
        if amount <= 0.0 or not values:
            return values
        grid = self._mutator_timing_grid(values)
        max_steps = max(1, min(16, int(math.ceil((float(loop_length) / grid) * amount))))
        step_choices = [step for step in range(-max_steps, max_steps + 1) if step != 0]
        if not step_choices:
            return values
        offset = rnd.choice(step_choices) * grid
        loop_length = max(0.0001, float(loop_length))
        for value in values:
            value["start"] = self._mutator_quantized_time((float(value.get("start", 0.0)) + offset) % loop_length, loop_length, grid)
        return values

    def _mutator_add_values_by_depth(self, values, depth, strength, role, loop_length, settings, rnd, rhythm=False, target_pitches=None):
        values = [dict(value) for value in tuple(values or ())]
        amount = max(0.0, min(1.0, float(depth)))
        if amount <= 0.0 or not values:
            return values
        rnd = random.Random((rnd.randint(0, 2147483647) << 1) ^ rnd.randint(0, 2147483647))
        loop_length = max(0.0001, float(loop_length))
        grid = self._mutator_addition_grid(values)
        cell_size = 1.0
        cell_count = max(1, int(math.ceil(loop_length / cell_size)))
        result = list(values)
        source_pitches = sorted(set(int(value.get("pitch", 60)) for value in values if 0 <= int(value.get("pitch", 60)) <= 127))
        target_pitch_set = set(int(pitch) for pitch in tuple(target_pitches or ()) if 0 <= int(pitch) <= 127)
        allowed_pitches = set(source_pitches)
        if rhythm and target_pitch_set:
            allowed_pitches = allowed_pitches.intersection(target_pitch_set) or allowed_pitches
        motif = [
            value for value in sorted(
                values,
                key=lambda item: (
                    float(item.get("start", 0.0)),
                    int(item.get("pitch", 60)),
                    -int(item.get("velocity", 96)),
                )
            )
            if int(value.get("pitch", 60)) in allowed_pitches
        ]
        if not motif:
            return values
        minimum_duration = 0.03125 if rhythm else 0.0625
        source_durations = sorted(set(
            max(minimum_duration, min(loop_length, quantized_duration))
            for quantized_duration in (
                round(max(minimum_duration, float(value.get("duration", 0.125))) / 0.03125) * 0.03125
                for value in values
            )
            if quantized_duration > 0.0
        ))

        def quantized(value):
            return max(0.0, min(loop_length - 0.0001, round(float(value) / grid) * grid))

        def quantized_to(value, step):
            step = max(0.0001, float(step))
            return max(0.0, min(loop_length - 0.0001, round(float(value) / step) * step))

        def cell_index(start):
            return max(0, min(cell_count - 1, int(math.floor(max(0.0, float(start)) / cell_size))))

        offsets_by_pitch = {}
        cells_by_pitch = {}
        occupied_starts = set()
        for value in values:
            pitch = int(value.get("pitch", 60))
            start = quantized(float(value.get("start", 0.0)))
            occupied_starts.add(int(round(start * 960.0)))
            cell = cell_index(start)
            offset = max(0.0, min(cell_size - grid, quantized(start - (cell * cell_size))))
            offsets_by_pitch.setdefault(pitch, set()).add(offset)
            cells_by_pitch.setdefault(pitch, set()).add(cell)

        def relevant_for_overlap(value, candidate_pitch):
            return int(value.get("pitch", 60)) == int(candidate_pitch)

        def start_is_taken(candidate_start, candidate_pitch):
            for value in result:
                if relevant_for_overlap(value, candidate_pitch) and abs(float(value.get("start", 0.0)) - candidate_start) < 0.000001:
                    return True
            return False

        def global_start_is_taken(candidate_start):
            return int(round(float(candidate_start) * 960.0)) in occupied_starts

        def max_free_duration(candidate_start, candidate_pitch):
            limit = max(0.0, loop_length - candidate_start)
            for value in result:
                if not relevant_for_overlap(value, candidate_pitch):
                    continue
                other_start = float(value.get("start", 0.0))
                other_duration = max(0.0001, float(value.get("duration", 0.0001)))
                other_end = other_start + other_duration
                if other_start <= candidate_start < other_end - 0.000001:
                    return 0.0
                if other_start > candidate_start + 0.000001:
                    limit = min(limit, other_start - candidate_start)
            return max(0.0, limit)

        def ordered_cells_for(base_cell):
            base_bar = base_cell // 4
            base_beat = base_cell % 4
            preferred = []
            answer_beats = ((base_beat - 1) % 4, (base_beat + 1) % 4, (base_beat + 2) % 4, base_beat, 0, 2, 1, 3)
            for bar_delta in (1, -1, 2, -2, 3, -3):
                for beat in answer_beats:
                    preferred.append(((base_bar + bar_delta) * 4) + beat)
            for beat in answer_beats:
                preferred.append((base_bar * 4) + beat)
            preferred.extend(range(cell_count))

            seen = set()
            cells = []
            for cell in preferred:
                if 0 <= cell < cell_count and cell != base_cell and cell not in seen:
                    seen.add(cell)
                    cells.append(cell)
            return cells

        def placement_candidates(base):
            pitch = int(base.get("pitch", 60))
            start = quantized(float(base.get("start", 0.0)))
            base_cell = cell_index(start)
            base_offset = max(0.0, min(cell_size - grid, quantized(start - (base_cell * cell_size))))
            offsets = sorted(offsets_by_pitch.get(pitch, {base_offset}), key=lambda offset: (abs(offset - base_offset), offset))
            pitch_cells = cells_by_pitch.get(pitch, set())
            scored = []
            for order_index, cell in enumerate(ordered_cells_for(base_cell)):
                for offset in offsets:
                    candidate_start = quantized((cell * cell_size) + offset)
                    if candidate_start >= loop_length - 0.000001:
                        continue
                    score = float(order_index)
                    if global_start_is_taken(candidate_start):
                        score += 2.5
                    if cell in pitch_cells:
                        score -= 0.35
                    if (cell % 4) == (base_cell % 4):
                        score -= 0.25
                    scored.append((score + rnd.random() * 0.01, candidate_start))
            return [start for _, start in sorted(scored, key=lambda item: item[0])]

        def fallback_gap_candidates(base):
            pitch = int(base.get("pitch", 60))
            base_start = quantized(float(base.get("start", 0.0)))
            base_cell = cell_index(base_start)
            steps = tuple(sorted(set([grid, max(grid, 0.5), max(grid, 1.0)])))
            intervals = []
            for value in result:
                if not relevant_for_overlap(value, pitch):
                    continue
                start = max(0.0, min(loop_length, float(value.get("start", 0.0))))
                end = max(start, min(loop_length, start + max(0.0001, float(value.get("duration", 0.0001)))))
                intervals.append((start, end))
            intervals.sort()

            free_spans = []
            cursor = 0.0
            for start, end in intervals:
                if start > cursor + 0.000001:
                    free_spans.append((cursor, start))
                cursor = max(cursor, end)
            if cursor < loop_length - 0.000001:
                free_spans.append((cursor, loop_length))

            scored = []
            for span_start, span_end in free_spans:
                if span_end - span_start < 0.03125:
                    continue
                span_mid = (span_start + span_end) * 0.5
                for step_index, step in enumerate(steps):
                    start = math.ceil((span_start + 0.000001) / step) * step
                    while start < span_end - 0.000001:
                        candidate_start = quantized_to(start, step)
                        if candidate_start >= span_end - 0.000001:
                            break
                        candidate_cell = cell_index(candidate_start)
                        score = (
                            step_index * 12.0
                            + abs(candidate_cell - base_cell)
                            + abs(candidate_start - span_mid) * 0.1
                        )
                        if (candidate_cell % 4) == (base_cell % 4):
                            score -= 0.2
                        scored.append((score + rnd.random() * 0.01, candidate_start))
                        start += step
            return [start for _, start in sorted(scored, key=lambda item: item[0])]

        def choose_start_and_duration(base):
            pitch = int(base.get("pitch", 60))
            base_duration = max(minimum_duration, float(base.get("duration", 0.125)))
            candidates = []
            seen = set()
            smart_candidates = self._mutator_phrase_start_candidates(
                motif,
                result,
                base,
                pitch,
                loop_length,
                rnd,
                grid,
                prefer_global_open=False
            )
            for candidate in list(smart_candidates) + list(placement_candidates(base)) + list(fallback_gap_candidates(base)):
                key = int(round(candidate / 0.0001))
                if key in seen:
                    continue
                seen.add(key)
                candidates.append(candidate)
            viable = []
            globally_free = []
            for candidate_start in candidates:
                if start_is_taken(candidate_start, pitch):
                    continue
                free_duration = max_free_duration(candidate_start, pitch)
                if free_duration >= minimum_duration - 0.000001:
                    candidate = (candidate_start, free_duration)
                    if not global_start_is_taken(candidate_start):
                        globally_free.append(candidate)
                    viable.append(candidate)
                if len(viable) >= 16:
                    break
            preferred = globally_free or viable
            if preferred:
                if globally_free and viable and rnd.random() >= 0.92:
                    preferred = viable
                window = max(1, min(len(viable), 2 + int(round(amount * 8.0))))
                window = max(1, min(len(preferred), window))
                candidate_start, free_duration = rnd.choice(preferred[:window])
                duration_pool = [duration for duration in source_durations if duration <= free_duration + 0.000001]
                if duration_pool and rnd.random() < 0.82:
                    duration = rnd.choice(duration_pool)
                else:
                    duration = base_duration * rnd.choice([0.5, 0.75, 1.0, 1.25, 1.5, 2.0])
                return candidate_start, max(minimum_duration, min(free_duration, duration))
            return None, None

        motif_by_pitch = {}
        for value in motif:
            motif_by_pitch.setdefault(int(value.get("pitch", 60)), []).append(value)

        add_count = self._mutator_deterministic_count(len(motif), amount)
        selected_bases = list(motif)
        rnd.shuffle(selected_bases)
        selected_bases = selected_bases[:add_count]

        for base in selected_bases:
            base = dict(base)
            pitch = int(base.get("pitch", 60))
            if pitch not in allowed_pitches:
                continue
            start, duration = choose_start_and_duration(base)
            if start is None:
                continue
            traits = self._mutator_sample_note_traits(motif_by_pitch.get(pitch, motif), rnd, fallback=base, minimum_duration=minimum_duration, maximum_duration=duration)
            cell = cell_index(start)
            offset = max(0.0, min(cell_size - grid, quantized(start - (cell * cell_size))))
            offsets_by_pitch.setdefault(pitch, set()).add(offset)
            cells_by_pitch.setdefault(pitch, set()).add(cell)
            occupied_starts.add(int(round(start * 960.0)))
            result.append(dict(
                base,
                pitch=pitch,
                start=start,
                duration=traits["duration"],
                velocity=traits["velocity"],
                mute=traits["mute"],
                probability=traits["probability"],
            ))
        return result

    def _mutator_gate_remaining_for_index(self, values, index, loop_length):
        start = max(0.0, min(float(loop_length) - 0.0001, float(values[index].get("start", 0.0))))
        pitch = int(values[index].get("pitch", 0))
        remaining = float(loop_length) - start
        for other_index, value in enumerate(values):
            if other_index == index or int(value.get("pitch", 0)) != pitch:
                continue
            other_start = float(value.get("start", 0.0))
            if other_start > start + 0.000001:
                remaining = min(remaining, other_start - start)
        return max(0.03125, remaining)

    def _mutator_apply_gate_by_depth(self, values, probability, strength, loop_length, rnd):
        values = [dict(value) for value in tuple(values or ())]
        for index in self._mutator_operation_indexes(len(values), probability, rnd):
            values[index]["duration"] = self._mutator_gate_duration(
                float(values[index].get("duration", 0.125)),
                self._mutator_gate_remaining_for_index(values, index, loop_length),
                strength,
                rnd
            )
        return values

    def _mutator_apply_octave_by_depth(self, values, depth, rnd):
        values = [dict(value) for value in tuple(values or ())]
        for index in self._mutator_operation_indexes(len(values), depth, rnd):
            values[index]["pitch"] = max(0, min(127, int(values[index].get("pitch", 60)) + rnd.choice([-12, 12])))
        return values

    def _mutator_apply_pitch_by_depth(self, values, probability, strength, settings, rnd, rhythm=False, target_pitches=None):
        values = [dict(value) for value in tuple(values or ())]
        amount = max(0.0, min(1.0, float(strength)))
        if rhythm:
            targets = sorted(set(int(pitch) for pitch in tuple(target_pitches or ()) if 0 <= int(pitch) <= 127))
            for index in self._mutator_deterministic_indexes(len(values), probability, rnd):
                pitch = int(values[index].get("pitch", 60))
                choices = [target for target in targets if target != pitch]
                if not choices:
                    continue
                choices.sort(key=lambda target: abs(target - pitch))
                window = max(1, min(len(choices), int(math.ceil(1 + amount * min(5, len(choices))))))
                values[index]["pitch"] = rnd.choice(choices[:window])
            return values
        for index in self._mutator_deterministic_indexes(len(values), probability, rnd):
            pitch = int(values[index].get("pitch", 60))
            max_steps = self._mutator_scale_step_span_for_octave_fraction(pitch, amount, settings)
            step_choices = [step for step in range(-max_steps, max_steps + 1) if step != 0]
            if step_choices:
                values[index]["pitch"] = self._mutator_transpose_scale_steps(pitch, rnd.choice(step_choices), settings)
        return values

    def _mutator_apply_pitch_add_by_depth(self, values, probability, strength, loop_length, settings, rnd, rhythm=False, target_pitches=None):
        values = [dict(value) for value in tuple(values or ())]
        if not values:
            return values
        add_count = self._mutator_deterministic_count(len(values), probability)
        if add_count <= 0:
            return values

        amount = max(0.0, min(1.0, float(strength)))
        selected = list(values)
        rnd.shuffle(selected)
        selected = selected[:add_count]
        result = list(values)
        loop_length = max(0.0001, float(loop_length))
        grid = self._mutator_timing_grid(values)
        occupied = set(
            (
                int(value.get("pitch", 60)),
                int(round(float(value.get("start", 0.0)) * 960.0))
            )
            for value in result
        )
        occupied_starts = set(int(round(float(value.get("start", 0.0)) * 960.0)) for value in result)
        targets = sorted(set(int(pitch) for pitch in tuple(target_pitches or ()) if 0 <= int(pitch) <= 127))

        def quantized_start(value):
            return self._mutator_quantized_time(value, loop_length, grid)

        def global_start_is_taken(start):
            return int(round(float(start) * 960.0)) in occupied_starts

        def pitch_start_is_taken(pitch, start):
            return (int(pitch), int(round(float(start) * 960.0))) in occupied

        for base in selected:
            base_start = max(0.0, min(loop_length - 0.0001, float(base.get("start", 0.0))))
            source_pitch = int(base.get("pitch", 60))
            pitch = None
            if rhythm and targets:
                choices = [target for target in targets if target != source_pitch]
                choices.sort(key=lambda target: abs(target - source_pitch))
                window = max(1, min(len(choices), int(math.ceil(1 + amount * min(6, len(choices)))))) if choices else 0
                if window:
                    pitch = rnd.choice(choices[:window])
            else:
                max_steps = self._mutator_scale_step_span_for_octave_fraction(source_pitch, amount, settings)
                step_choices = [step for step in range(-max_steps, max_steps + 1) if step != 0]
                rnd.shuffle(step_choices)
                for step in step_choices:
                    candidate = self._mutator_transpose_scale_steps(source_pitch, step, settings)
                    if candidate != source_pitch:
                        pitch = candidate
                        break
            if pitch is None:
                continue

            start = None
            candidate_starts = self._mutator_phrase_start_candidates(
                values,
                result,
                base,
                pitch,
                loop_length,
                rnd,
                grid,
                prefer_global_open=True
            )
            for candidate_start in candidate_starts:
                if not pitch_start_is_taken(pitch, candidate_start):
                    start = candidate_start
                    if not global_start_is_taken(candidate_start):
                        break
            if start is not None and global_start_is_taken(start):
                for candidate_start in candidate_starts:
                    if not global_start_is_taken(candidate_start) and not pitch_start_is_taken(pitch, candidate_start):
                        start = candidate_start
                        break
            if start is None:
                steps = max(1, int(round(loop_length / grid)))
                base_step = int(round(quantized_start(base_start) / grid)) % steps
                for offset in (1, 2, 3, 4, -1, -2, 6, -4, 8, -6, 12, -8):
                    candidate_start = quantized_start(((base_step + offset) % steps) * grid)
                    if not global_start_is_taken(candidate_start) and not pitch_start_is_taken(pitch, candidate_start):
                        start = candidate_start
                        break
            if start is None:
                fallback_start = quantized_start(base_start)
                if not pitch_start_is_taken(pitch, fallback_start):
                    start = fallback_start
            if start is None:
                continue
            occupied.add((int(pitch), int(round(start * 960.0))))
            occupied_starts.add(int(round(start * 960.0)))
            traits = self._mutator_sample_note_traits(values, rnd, fallback=base, minimum_duration=0.03125 if rhythm else 0.0625, maximum_duration=loop_length - start)
            result.append(dict(
                base,
                pitch=max(0, min(127, int(pitch))),
                start=start,
                duration=traits["duration"],
                velocity=traits["velocity"],
                mute=traits["mute"],
                probability=traits["probability"],
            ))
        return result

    def _mutator_duplicate_by_depth(self, values, probability, loop_length, rnd):
        values = [dict(value) for value in tuple(values or ())]
        indexes = self._mutator_deterministic_indexes(len(values), probability, rnd)
        if not indexes:
            return values
        selected = [dict(values[index]) for index in indexes]
        loop_length = max(0.0001, float(loop_length))
        grid = self._mutator_pack_grid(selected)
        ticks_per_beat = 960
        loop_ticks = max(1, int(round(loop_length * ticks_per_beat)))
        grid_ticks = max(1, int(round(grid * ticks_per_beat)))

        def snap_tick(start):
            tick = int(round(float(start) * ticks_per_beat))
            return int(round(float(tick) / float(grid_ticks))) * grid_ticks

        snapped_starts = [max(0, min(loop_ticks - 1, snap_tick(value.get("start", 0.0)))) for value in selected]
        range_start_tick = (min(snapped_starts) // grid_ticks) * grid_ticks
        range_end_tick = (((max(snapped_starts) + grid_ticks) + grid_ticks - 1) // grid_ticks) * grid_ticks
        range_start_tick = max(0, min(loop_ticks - 1, range_start_tick))
        range_end_tick = max(range_start_tick + grid_ticks, min(loop_ticks, range_end_tick))
        range_length_ticks = max(grid_ticks, range_end_tick - range_start_tick)
        max_steps = max(1, min(32, int(math.ceil(float(loop_ticks) / float(range_length_ticks)))))
        copy_count = 1
        result = list(values)

        def shifted_note(value, step):
            relative_tick = snap_tick(value.get("start", 0.0)) - range_start_tick
            start_tick = (range_start_tick + relative_tick + (step * range_length_ticks)) % loop_ticks
            start = max(0.0, min(loop_length - 0.0001, float(start_tick) / float(ticks_per_beat)))
            return dict(
                value,
                start=start,
                duration=max(0.03125, min(float(value.get("duration", 0.03125)), loop_length - start))
            )

        logical_steps = [
            step
            for step in range(1, max_steps + 1)
            if (step * range_length_ticks) % loop_ticks != 0
        ]
        if len(logical_steps) < copy_count:
            logical_steps.extend(
                -step
                for step in range(1, max_steps + 1)
                if (-step * range_length_ticks) % loop_ticks != 0
            )

        used_steps = []
        for step in logical_steps:
            if step in used_steps:
                continue
            used_steps.append(step)
            for candidate in [shifted_note(value, step) for value in selected]:
                result.append(candidate)
            if len(used_steps) >= copy_count:
                break

        if used_steps:
            return result

        offsets = [1, 2, 3, 4, 6, 8, 12, 16, -1, -2, -3, -4, -6, -8, -12, -16]
        rnd.shuffle(offsets)
        offsets.sort(key=lambda step: (abs(step), 0 if step > 0 else 1))
        for repeat_index in range(copy_count):
            offset = offsets[repeat_index % len(offsets)] * grid_ticks
            for value in selected:
                start_tick = (snap_tick(value.get("start", 0.0)) + offset) % loop_ticks
                start = max(0.0, min(loop_length - 0.0001, float(start_tick) / float(ticks_per_beat)))
                result.append(dict(
                    value,
                    start=start,
                    duration=max(0.03125, min(float(value.get("duration", 0.03125)), loop_length - start))
                ))
        return result

    def _mutator_phrase_shift_by_depth(self, values, probability, strength, settings, rnd, rhythm=False, target_pitches=None):
        values = [dict(value) for value in tuple(values or ())]
        indexes = self._mutator_deterministic_indexes(len(values), probability, rnd)
        if not indexes:
            return values
        amount = max(0.0, min(1.0, float(strength)))
        if rhythm:
            targets = sorted(set(int(pitch) for pitch in tuple(target_pitches or ()) if 0 <= int(pitch) <= 127))
            if len(targets) < 2:
                return values
            selected_positions = [
                targets.index(int(values[index].get("pitch", 60)))
                for index in indexes
                if int(values[index].get("pitch", 60)) in targets
            ]
            if not selected_positions:
                return values
            min_position = min(selected_positions)
            max_position = max(selected_positions)
            max_down = min_position
            max_up = len(targets) - 1 - max_position
            max_span = max(max_down, max_up)
            if max_span <= 0:
                return values
            max_steps = max(1, min(max_span, int(math.ceil(max_span * max(0.125, amount)))))
            step_choices = [step for step in range(-max_steps, max_steps + 1) if step != 0 and -max_down <= step <= max_up]
            if not step_choices:
                return values
            offset = rnd.choice(step_choices)
            for index in indexes:
                pitch = int(values[index].get("pitch", 60))
                if pitch not in targets:
                    continue
                values[index]["pitch"] = targets[targets.index(pitch) + offset]
            return values

        selected_pitches = [int(values[index].get("pitch", 60)) for index in indexes]
        if not selected_pitches:
            return values
        max_steps = min(
            self._mutator_scale_step_span_for_octave_fraction(min(selected_pitches), amount, settings),
            self._mutator_scale_step_span_for_octave_fraction(max(selected_pitches), amount, settings)
        )
        step_choices = [step for step in range(-max_steps, max_steps + 1) if step != 0]
        if not step_choices:
            return values
        offset = rnd.choice(step_choices)
        for index in indexes:
            values[index]["pitch"] = self._mutator_transpose_scale_steps(int(values[index].get("pitch", 60)), offset, settings)
        return values

    def _mutator_preserve_original_by_depth(self, values, source_values, probability, loop_length, rnd):
        values = [dict(value) for value in tuple(values or ())]
        source = [dict(value) for value in tuple(source_values or ())]
        indexes = self._mutator_deterministic_indexes(len(source), probability, rnd)
        if not indexes:
            return values
        loop_length = max(0.0001, float(loop_length))
        preserved = []
        for index in indexes:
            note = dict(source[index])
            start = max(0.0, min(loop_length - 0.0001, float(note.get("start", 0.0))))
            note["start"] = start
            note["duration"] = max(0.03125, min(float(note.get("duration", 0.03125)), loop_length - start))
            preserved.append(note)
        result = []
        for value in values:
            if any(self._mutator_notes_overlap(value, note) for note in preserved):
                continue
            result.append(value)
        result.extend(preserved)
        return result

    def _mutator_reverse_timing_by_depth(self, values, probability, loop_length, rnd):
        values = [dict(value) for value in tuple(values or ())]
        indexes = self._mutator_deterministic_indexes(len(values), probability, rnd)
        if len(indexes) < 2:
            return values
        selected = [values[index] for index in indexes]
        range_start = min(float(value.get("start", 0.0)) for value in selected)
        range_end = max(float(value.get("start", 0.0)) + max(0.0001, float(value.get("duration", 0.0001))) for value in selected)
        range_end = min(max(range_start + 0.0001, range_end), float(loop_length))
        for index in indexes:
            duration = max(0.0001, float(values[index].get("duration", 0.0001)))
            original_end = float(values[index].get("start", 0.0)) + duration
            values[index]["start"] = max(0.0, min(float(loop_length) - 0.0001, range_start + (range_end - original_end)))
        return values

    def _mutator_invert_pitch_by_depth(self, values, probability, settings, rnd, rhythm=False, target_pitches=None):
        values = [dict(value) for value in tuple(values or ())]
        indexes = self._mutator_deterministic_indexes(len(values), probability, rnd)
        if len(indexes) < 2:
            return values
        selected_pitches = [int(values[index].get("pitch", 60)) for index in indexes]
        min_pitch = min(selected_pitches)
        max_pitch = max(selected_pitches)
        if rhythm:
            allowed = sorted(set(int(pitch) for pitch in tuple(target_pitches or selected_pitches) if 0 <= int(pitch) <= 127))
            if len(allowed) < 2:
                return values
            selected_indexes = [allowed.index(pitch) for pitch in selected_pitches if pitch in allowed]
            if len(selected_indexes) < 2:
                return values
            min_index = min(selected_indexes)
            max_index = max(selected_indexes)
            for index in indexes:
                pitch = int(values[index].get("pitch", 60))
                if pitch not in allowed:
                    continue
                mirrored_index = max_index - max(0, allowed.index(pitch) - min_index)
                if 0 <= mirrored_index < len(allowed):
                    values[index]["pitch"] = allowed[mirrored_index]
            return values
        if settings.get("scale", "Minor") == "Chromatic":
            for index in indexes:
                pitch = int(values[index].get("pitch", 60))
                values[index]["pitch"] = max(0, min(127, min_pitch + (max_pitch - pitch)))
            return values

        allowed = self._mutator_scale_notes(settings)
        try:
            min_index = self._mutator_scale_index(min_pitch, allowed)
            max_index = self._mutator_scale_index(max_pitch, allowed)
        except Exception:
            return values
        for index in indexes:
            pitch_index = self._mutator_scale_index(int(values[index].get("pitch", 60)), allowed)
            mirrored_index = max_index - max(0, pitch_index - min_index)
            if 0 <= mirrored_index < len(allowed):
                values[index]["pitch"] = allowed[mirrored_index]
        return values

    def _mutator_remove_by_depth(self, values, depth, strength, rnd):
        values = [dict(value) for value in tuple(values or ())]
        if max(0.0, min(1.0, float(depth))) <= 0.0:
            return values
        remove_indexes = set(self._mutator_deterministic_indexes(len(values), depth, rnd))
        if not remove_indexes:
            return values
        return [value for index, value in enumerate(values) if index not in remove_indexes]

    def _mutator_apply_velocity_by_depth(self, values, probability, strength, role, rnd, rhythm=False):
        values = [dict(value) for value in tuple(values or ())]
        span = max(1, int(round(max(0.0, min(1.0, float(strength))) * 127.0)))
        for index in self._mutator_operation_indexes(len(values), probability, rnd):
            velocity = int(values[index].get("velocity", 96))
            values[index]["velocity"] = max(1, min(127, velocity + rnd.randint(-span, span)))
        return values

    def _mutator_apply_role_shape(self, values, role, loop_length, settings, rnd, rhythm=False):
        result = [dict(value) for value in tuple(values or ())]
        slot_shape_depth = 0.0
        for slot in self._mutator_active_slots(settings):
            slot_shape_depth = max(slot_shape_depth, self._mutator_depth_value(slot, "probability_depth", 0.0))
        shape_depth = max(
            slot_shape_depth,
            self._mutator_depth_value(settings, "rhythmic_shift_depth"),
            self._mutator_depth_value(settings, "note_addition_depth"),
            self._mutator_depth_value(settings, "note_removal_depth"),
            self._mutator_depth_value(settings, "velocity_change_depth"),
            self._mutator_depth_value(settings, "gate_change_depth"),
            self._mutator_depth_value(settings, "shift_depth"),
            0.0 if rhythm else self._mutator_depth_value(settings, "octave_shift_depth"),
            0.0 if rhythm else self._mutator_depth_value(settings, "pitch_shift_depth"),
        )
        if shape_depth <= 0.0 and role != 15:
            return result
        if role == 6:
            for index, value in enumerate(result):
                value["start"] = round(float(value.get("start", 0.0)) / 0.5) * 0.5
                if index % 2 == 0:
                    value["velocity"] = min(127, int(value.get("velocity", 96)) + 10)
        elif role == 4:
            for value in result:
                if float(value.get("start", 0.0)) > float(loop_length) * 0.65:
                    value["pitch"] = self._mutator_quantize_pitch(int(value.get("pitch", 60)) + (2 if rnd.random() < 0.6 else -2), settings)
                    value["velocity"] = min(127, int(value.get("velocity", 96)) + 8)
        elif role == 13:
            for value in result:
                value["velocity"] = min(127, int(value.get("velocity", 96)) + 6)
        elif role == 15:
            for index, value in enumerate(result):
                value["start"] = round(float(value.get("start", 0.0)) / 0.25) * 0.25
                value["velocity"] = min(127, int(value.get("velocity", 96)) + 18)
                if index % 2 == 0:
                    value["pitch"] = max(0, min(127, int(value.get("pitch", 60)) + 12))
        return result

    def _mutator_apply_add_shift_by_depth(self, values, source_values, role, loop_length, settings, rnd, depth, rhythm=False, target_pitches=None):
        depth = max(0.0, min(1.0, float(depth)))
        result = [dict(value) for value in tuple(values or ())]
        source = [dict(value) for value in tuple(source_values or ())]
        if depth <= 0.0 or not result:
            return result

        if rhythm:
            targets = sorted(set(int(pitch) for pitch in tuple(target_pitches or ()) if 0 <= int(pitch) <= 127))
            if targets:
                for index in self._mutator_operation_indexes(len(result), depth, rnd):
                    pitch = int(result[index].get("pitch", 60))
                    choices = [target for target in targets if target != pitch]
                    if choices:
                        choices.sort(key=lambda target: abs(target - pitch))
                        window = max(1, min(len(choices), int(math.ceil(1 + depth * min(5, len(choices))))))
                        result[index]["pitch"] = rnd.choice(choices[:window])
        else:
            for value in result:
                if rnd.random() < depth:
                    max_steps = max(1, min(4, int(math.ceil(depth * 4))))
                    step_choices = list(range(-max_steps, 0)) + list(range(1, max_steps + 1))
                    value["pitch"] = self._mutator_transpose_scale_steps(
                        int(value.get("pitch", 60)),
                        rnd.choice(step_choices),
                        settings
                    )

        if role in (0, 8, 14) or not source:
            return result

        additions = int(math.ceil(depth * int(settings.get("mutations_per_pass", 1)) * (4 if role in (4, 7) else 3)))
        if depth >= 0.18 and role in (1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 15):
            additions = max(1, additions)
        if depth >= 0.55:
            additions = max(additions, min(len(set(value.get("pitch", 60) for value in source)) + 1, 6))
        if depth >= 0.80:
            additions = max(additions, min(len(set(value.get("pitch", 60) for value in source)) * 2, 12))
        if additions <= 0:
            return result

        allowed_pitches = set(int(value.get("pitch", 60)) for value in source)
        if rhythm and target_pitches:
            targets = set(int(pitch) for pitch in tuple(target_pitches or ()) if 0 <= int(pitch) <= 127)
            allowed_pitches = allowed_pitches.intersection(targets) or allowed_pitches
        motif = [value for value in source if int(value.get("pitch", 60)) in allowed_pitches]
        motif.sort(key=lambda item: int(item.get("velocity", 96)), reverse=True)
        grid = 0.0625 if role == 7 else self._mutator_timing_grid(source or result)
        occupied = self._mutator_rhythm_occupied_steps(result, loop_length, grid=grid)
        for add_index in range(additions):
            if not motif:
                break
            base = dict(motif[add_index % len(motif)])
            if role == 7:
                start = float(loop_length) * rnd.choice([0.75, 0.8125, 0.875, 0.9375])
                duration = min(0.25, float(loop_length) - start)
            else:
                start = (float(base.get("start", 0.0)) + rnd.choice([0.125, 0.25, 0.375, 0.5, 0.75, 1.0])) % float(loop_length)
                duration = min(float(base.get("duration", 0.125)), float(loop_length) - start)
            pitch = int(base.get("pitch", 60))
            start_candidates = self._mutator_phrase_start_candidates(
                motif,
                result,
                base,
                pitch,
                loop_length,
                rnd,
                grid,
                prefer_global_open=False
            )
            if role == 7:
                start_candidates = [
                    candidate
                    for candidate in start_candidates
                    if candidate >= max(0.0, float(loop_length) - 1.0)
                ] or start_candidates
            start_candidates.append(start)
            placed_start = None
            for candidate in start_candidates[:18]:
                placed_start = self._mutator_rhythm_free_start(pitch, candidate, loop_length, occupied, grid=grid)
                if placed_start is not None:
                    break
            start = placed_start
            if start is None:
                continue
            duration_limit = min(duration, max(0.03125, float(loop_length) - start))
            traits = self._mutator_sample_note_traits(motif, rnd, fallback=base, minimum_duration=0.03125 if rhythm else 0.0625, maximum_duration=duration_limit)
            result.append(dict(
                base,
                pitch=pitch,
                start=start,
                duration=traits["duration"],
                velocity=traits["velocity"],
                mute=traits["mute"],
                probability=traits["probability"],
            ))
        return result

    def _mutator_apply_depth_pipeline(self, values, role, loop_length, settings, rnd, rhythm=False, target_pitches=None):
        source_values = [dict(value) for value in tuple(values or ())]
        result = [dict(value) for value in source_values]
        if role == 14:
            return self._mutator_apply_break_role(result, loop_length, rhythm=rhythm)

        result = self._mutator_apply_role_shape(result, role, loop_length, settings, rnd, rhythm=rhythm)
        keys = self._mutator_operation_depth_keys()
        for slot in self._mutator_active_slots(settings):
            operation_index = int(slot.get("operation", 0))
            probability = self._mutator_role_operation_depth(role, self._mutator_depth_value(slot, "probability_depth", 0.0))
            strength = self._mutator_depth_value(slot, "range_depth", 0.5)
            if probability <= 0.0:
                continue
            if operation_index == self._mutator_add_shift_operation_index():
                result = self._mutator_apply_add_shift_by_depth(result, source_values, role, loop_length, settings, rnd, probability, rhythm=rhythm, target_pitches=target_pitches)
                continue
            if operation_index == self._mutator_loop_shift_operation_index():
                result = self._mutator_loop_shift_by_depth(result, probability, loop_length, rnd)
                continue
            if operation_index == self._mutator_reverse_operation_index():
                result = self._mutator_reverse_timing_by_depth(result, probability, loop_length, rnd)
                continue
            if operation_index == self._mutator_invert_operation_index():
                result = self._mutator_invert_pitch_by_depth(result, probability, settings, rnd, rhythm=rhythm, target_pitches=target_pitches)
                continue
            if operation_index == self._mutator_pitch_add_operation_index():
                result = self._mutator_apply_pitch_add_by_depth(result, probability, strength, loop_length, settings, rnd, rhythm=rhythm, target_pitches=target_pitches)
                continue
            if operation_index == self._mutator_duplicate_operation_index():
                result = self._mutator_duplicate_by_depth(result, probability, loop_length, rnd)
                continue
            if operation_index == self._mutator_phrase_shift_operation_index():
                result = self._mutator_phrase_shift_by_depth(result, probability, strength, settings, rnd, rhythm=rhythm, target_pitches=target_pitches)
                continue
            if operation_index == self._mutator_preserver_operation_index():
                result = self._mutator_preserve_original_by_depth(result, source_values, probability, loop_length, rnd)
                continue
            if operation_index < 0 or operation_index >= len(keys):
                continue
            key = keys[operation_index]
            if key == "fill_depth":
                self._mutator_add_fill_values(result, result, role, loop_length, settings, rnd, target_pitches=target_pitches, rhythm=rhythm, fill_depth=probability)
            elif key == "simplification_depth":
                result = self._mutator_simplify_values(result, probability, rnd)
            elif key == "rhythmic_shift_depth":
                result = self._mutator_shift_starts_by_depth(result, probability, strength, loop_length, rnd, rhythm=rhythm)
            elif key == "shift_depth":
                result = self._mutator_shift_pitch_groups_by_depth(result, probability, strength, loop_length, rnd, rhythm=rhythm, target_pitches=target_pitches)
            elif key == "note_addition_depth":
                result = self._mutator_add_values_by_depth(result, probability, strength, role, loop_length, settings, rnd, rhythm=rhythm, target_pitches=target_pitches)
            elif key == "gate_change_depth":
                result = self._mutator_apply_gate_by_depth(result, probability, strength, loop_length, rnd)
            elif key == "octave_shift_depth" and not rhythm:
                result = self._mutator_apply_octave_by_depth(result, probability, rnd)
            elif key == "pitch_shift_depth":
                result = self._mutator_apply_pitch_by_depth(result, probability, strength, settings, rnd, rhythm=rhythm, target_pitches=target_pitches)
            elif key == "note_removal_depth":
                result = self._mutator_remove_by_depth(result, probability, strength, rnd)
            elif key == "velocity_change_depth":
                result = self._mutator_apply_velocity_by_depth(result, probability, strength, role, rnd, rhythm=rhythm)
        return self._mutator_algorithm_prune_values(result, loop_length)

    def _mutator_add_fill_values(self, result, values, role, loop_length, settings, rnd, target_pitches=None, rhythm=False, fill_depth=None):
        if role not in (4, 5, 6, 7, 13, 14, 15):
            return
        pool = [dict(value) for value in tuple(values or ())]
        if target_pitches:
            targets = [int(pitch) for pitch in target_pitches if 0 <= int(pitch) <= 127]
            pool = [value for value in pool if int(value.get("pitch", 0)) in targets] or [
                dict(pitch=pitch, start=0.0, duration=0.125, velocity=96, mute=False, probability=1.0)
                for pitch in targets
            ]
        else:
            targets = sorted(set(int(value.get("pitch", 0)) for value in pool))
        if not pool or not targets:
            return

        if fill_depth is not None:
            fill_depth = max(0.0, min(1.0, float(fill_depth)))
        elif settings.get("algorithm", "mutator") == "mutator":
            fill_depth = self._mutator_role_operation_depth(role, self._mutator_depth_value(settings, "fill_depth", 0.0))
        else:
            fill_depth = max(0.0, min(1.0, float(settings.get("depth", 0.0))))
        if fill_depth <= 0.0:
            return
        hit_count = max(1, min(8, int(math.ceil(fill_depth * (8 if rhythm else 7)))))
        if hit_count <= 2:
            fill_window = min(float(loop_length), 2.0)
        elif hit_count <= 4:
            fill_window = min(float(loop_length), 1.5)
        else:
            fill_window = min(float(loop_length), 1.0)
        fill_start = max(0.0, float(loop_length) - fill_window)
        fill_patterns = {
            1: ((0.5,), (1.0,), (1.5,)),
            2: ((0.0, 1.0), (0.5, 1.5), (0.5, 1.0), (1.0, 1.5)),
            3: ((0.0, 0.75, 1.25), (0.25, 0.75, 1.25), (0.5, 1.0, 1.25)),
            4: ((0.0, 0.5, 1.0, 1.25), (0.25, 0.75, 1.0, 1.25), (0.0, 0.5, 0.75, 1.25)),
            5: ((0.25, 0.5, 0.625, 0.75, 0.875), (0.375, 0.5, 0.625, 0.75, 0.875)),
            6: ((0.25, 0.375, 0.5, 0.625, 0.75, 0.875), (0.125, 0.375, 0.5, 0.625, 0.75, 0.875)),
            7: ((0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875),),
            8: ((0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875),),
        }
        offsets = list(rnd.choice(fill_patterns.get(hit_count, fill_patterns[8])))
        occupied = self._mutator_rhythm_occupied_steps(result, loop_length, grid=0.0625)
        contour_targets = list(targets)
        rnd.shuffle(contour_targets)

        for fill_index, offset in enumerate(offsets):
            base = dict(rnd.choice(pool))
            if rhythm:
                pitch = rnd.choice(targets)
            elif contour_targets:
                pitch = contour_targets[fill_index % len(contour_targets)]
            else:
                pitch = int(base.get("pitch", 60))
            start = fill_start + min(fill_window - 0.0625, offset)
            start = self._mutator_rhythm_free_start(pitch, start, loop_length, occupied, grid=0.0625)
            if start is None:
                continue
            traits = self._mutator_sample_note_traits(pool, rnd, fallback=base, minimum_duration=0.03125, maximum_duration=float(loop_length) - start)
            velocity = int(traits.get("velocity", base.get("velocity", 96)))
            velocity_depth = fill_depth
            if settings.get("algorithm", "mutator") == "mutator":
                velocity_depth = self._mutator_role_operation_depth(role, self._mutator_depth_value(settings, "velocity_change_depth", 0.0))
            if velocity_depth > 0.0:
                velocity += self._mutator_velocity_delta(rnd, velocity_depth, role, rhythm=rhythm)
            result.append(dict(
                base,
                pitch=pitch,
                start=start,
                duration=max(0.03125, min(traits["duration"], 0.125 if rhythm else 0.16, float(loop_length) - start)),
                velocity=max(1, min(127, velocity)),
                mute=traits["mute"],
                probability=traits["probability"],
            ))

    def _mutator_rhythm_named_values(self, role, loop_length, settings, rnd):
        algorithm = settings.get("algorithm", "mutator")
        if algorithm not in ("backbeat_engine", "broken_garage", "four_floor_bloom"):
            return None
        targets = self._mutator_rhythm_target_pitches(settings, [])
        if not targets:
            return []

        try:
            bar_beats = float(max(1, int(self.song().signature_numerator)))
        except Exception:
            bar_beats = 4.0
        bar_beats = min(max(1.0, bar_beats), max(1.0, float(loop_length)))
        total_bars = max(1, int(math.ceil(float(loop_length) / bar_beats)))
        kick = targets[0]
        snare = targets[min(1, len(targets) - 1)]
        hat = targets[min(2, len(targets) - 1)]
        open_hat = targets[min(3, len(targets) - 1)]
        result = []
        depth = max(0.0, min(1.0, float(settings.get("depth", 0.0))))

        for bar in range(total_bars):
            offset = bar * bar_beats
            if algorithm == "four_floor_bloom":
                kick_beats = [0.0, 1.0, 2.0, 3.0]
                snare_beats = [1.0, 3.0] if len(targets) > 1 else []
                hat_beats = [0.5, 1.5, 2.5, 3.5] if len(targets) > 2 else []
            elif algorithm == "broken_garage":
                kick_beats = [0.0, 1.5, 2.75]
                snare_beats = [1.0, 3.0] if len(targets) > 1 else []
                hat_beats = [0.0, 0.75, 1.5, 2.25, 3.25] if len(targets) > 2 else []
            else:
                kick_beats = [0.0, 2.0, 2.75 if (bar + role) % 2 else 3.0]
                snare_beats = [1.0, 3.0] if len(targets) > 1 else []
                hat_beats = [i * 0.5 for i in range(int(bar_beats * 2))] if len(targets) > 2 else []

            if role in (1, 2, 4, 13) and depth > 0.2:
                kick_beats = kick_beats + ([3.5] if bar % 2 == 1 else [])
            if role in (7, 14, 15):
                kick_beats = [beat for beat in kick_beats if beat < max(0.0, bar_beats - 1.0)]
                snare_beats = [beat for beat in snare_beats if beat < max(0.0, bar_beats - 1.0)]
                hat_beats = [beat for beat in hat_beats if beat < max(0.0, bar_beats - 1.0)]

            for beat in kick_beats:
                if beat < bar_beats:
                    result.append(dict(pitch=kick, start=offset + beat, duration=0.125, velocity=104, mute=False, probability=1.0))
            for beat in snare_beats:
                if beat < bar_beats:
                    result.append(dict(pitch=snare, start=offset + beat, duration=0.125, velocity=98, mute=False, probability=1.0))
            for beat in hat_beats:
                if beat < bar_beats:
                    pitch = open_hat if len(targets) > 3 and beat % 1.0 == 0.5 and algorithm == "four_floor_bloom" else hat
                    result.append(dict(pitch=pitch, start=offset + beat, duration=0.0625, velocity=70 if pitch == hat else 86, mute=False, probability=1.0))

        self._mutator_add_fill_values(result, result, role, loop_length, settings, rnd, target_pitches=targets, rhythm=True)

        if depth > 0.0:
            for value in result:
                if rnd.random() < depth:
                    start = max(0.0, min(float(loop_length) - 0.0001, float(value.get("start", 0.0))))
                    value["duration"] = self._mutator_rhythm_gate_duration(
                        float(value.get("duration", 0.125)),
                        float(loop_length) - start,
                        depth,
                        rnd
                    )

        return self._mutator_algorithm_prune_values(result, loop_length)

    def _mutator_make_rhythm_section_values(self, source_values, role, source_start, loop_length, settings, rnd):
        if role in (0, 8):
            return self._mutator_relative_section_values(source_values, source_start, loop_length)

        named_values = self._mutator_rhythm_named_values(role, loop_length, settings, rnd)
        if named_values is not None:
            return named_values

        values = self._mutator_relative_section_values(source_values, source_start, loop_length)
        values.sort(key=lambda item: (item["start"], item["pitch"]))
        if not values:
            for pitch in self._mutator_rhythm_target_pitches(settings, source_values):
                values.append(dict(pitch=pitch, start=0.0, duration=0.125, velocity=96, mute=False, probability=1.0))

        target_pitches = self._mutator_rhythm_target_pitches(settings, source_values)
        result = self._mutator_apply_depth_pipeline(
            values,
            role,
            loop_length,
            settings,
            rnd,
            rhythm=True,
            target_pitches=target_pitches
        )

        return self._mutator_algorithm_prune_values(result, loop_length)

    def _mutator_make_section_values(self, source_values, role, source_start, loop_length, settings, rnd):
        if settings.get("companion_mode", "melody") == "rhythm":
            return self._mutator_make_rhythm_section_values(source_values, role, source_start, loop_length, settings, rnd)

        if role in (0, 8):
            return [
                dict(
                    value,
                    start=max(0.0, min(loop_length - 0.0001, value["start"] - source_start)),
                    duration=min(value["duration"], loop_length - max(0.0, min(loop_length - 0.0001, value["start"] - source_start)))
                )
                for value in source_values
            ]

        algorithm_values = self._mutator_make_algorithm_section_values(source_values, role, source_start, loop_length, settings, rnd)
        if algorithm_values is not None:
            return algorithm_values

        values = self._mutator_relative_section_values(source_values, source_start, loop_length)
        values.sort(key=lambda item: (item["start"], item["pitch"]))
        section_values = self._mutator_apply_depth_pipeline(
            values,
            role,
            loop_length,
            settings,
            rnd,
            rhythm=False
        )

        return section_values

    def _mutator_scale_name_from_index(self, scale_index):
        try:
            index = max(0, min(len(self.MUTATOR_SCALE_NAMES) - 1, int(scale_index)))
            return self.MUTATOR_SCALE_NAMES[index]
        except Exception:
            return "Minor"

    def _mutator_settings_from_info(self, info, seed=None):
        companion_mode = info.get("companion_mode", "melody")

        settings = {
            "preset": int(info.get("settings_preset", info.get("preset", 9))),
            "mutations_per_pass": max(1, min(3, int(info.get("mutations_per_pass", 1)))),
            "regenerate_mode": int(info.get("regenerate_mode", 0)),
            "source_mode": int(info.get("source_mode", 2)),
            "depth": max(0.0, min(1.0, float(info.get("depth", 0.0)))),
            "scale": self._mutator_scale_name_from_index(info.get("scale_index", 2)),
            "root": max(0, min(11, int(info.get("root", 0)))),
            "seed": int(seed if seed is not None else info.get("seed", random.randint(1, 2000000000))),
            "algorithm": info.get("algorithm", "mutator"),
            "companion_mode": companion_mode,
            "target_pitches": info.get("target_pitches", []),
            "operation_order": info.get("operation_order", []),
            "mutator_slots": self._mutator_normalized_slots(info),
            "mutator_slot_count": self._mutator_slot_count_from_value(info.get("mutator_slot_count", ""), self._mutator_normalized_slots(info)),
        }
        for key in self._mutator_operation_depth_keys():
            settings[key] = self._mutator_depth_value(info, key, 0.0)
        return settings

    def _mutator_generation_is_busy(self, key):
        with self._mutator_generation_lock:
            return key in self._mutator_generation_in_progress or key in self._mutator_generation_scheduled

    def _queue_mutator_work(self, key, work):
        if not work:
            return False
        with self._mutator_generation_lock:
            self._queued_mutator_work[key] = work
        return True

    def _queue_mutator_settings_update(self, key, clip, settings, send_updates=True):
        return self._queue_mutator_work(key, {
            "type": "settings",
            "clip": clip,
            "settings": dict(settings or {}),
            "send_updates": bool(send_updates),
        })

    def _queue_mutator_generation(self, key, clip, settings, previous_info=None, send_updates=True):
        return self._queue_mutator_work(key, {
            "type": "generate",
            "clip": clip,
            "settings": dict(settings or {}),
            "previous_info": dict(previous_info or {}) if previous_info else None,
            "send_updates": bool(send_updates),
        })

    def _pop_queued_mutator_work(self, key):
        with self._mutator_generation_lock:
            return self._queued_mutator_work.pop(key, None)

    def _flush_queued_mutator_work(self, key):
        with self._mutator_generation_lock:
            if key in self._mutator_generation_in_progress or key in self._mutator_generation_scheduled:
                return False
        work = self._pop_queued_mutator_work(key)

        if not work:
            return False

        def run_queued_work():
            work_type = work.get("type")
            clip = work.get("clip")
            settings = work.get("settings", {})
            send_updates = bool(work.get("send_updates", True))
            if work_type == "generate":
                previous_info = self._mutator_info(clip) or work.get("previous_info")
                self._generate_mutator_clip(
                    clip,
                    settings,
                    previous_info=previous_info,
                    send_updates=send_updates,
                    automatic=False
                )
            elif work_type == "settings":
                self._apply_mutator_clip_settings(clip, settings, send_updates=send_updates)

        try:
            self.schedule_message(1, run_queued_work)
            return True
        except Exception:
            try:
                run_queued_work()
                return True
            except Exception as e:
                self._debug_log("Error flushing queued mutator work: {}".format(str(e)))
                return False

    def _begin_selected_clip_update_batch(self):
        self._selected_clip_update_suppression_depth += 1

    def _end_selected_clip_update_batch(self):
        self._selected_clip_update_suppression_depth = max(0, self._selected_clip_update_suppression_depth - 1)
        if self._selected_clip_update_suppression_depth == 0:
            self._selected_clip_update_pending_metadata = False
            self._selected_clip_update_pending_notes = False

    def _selected_clip_updates_are_suppressed(self):
        return self._selected_clip_update_suppression_depth > 0

    def _begin_undo_step(self):
        try:
            song = self.song()
            begin_undo_step = getattr(song, "begin_undo_step", None)
            if begin_undo_step:
                begin_undo_step()
                return True
        except Exception:
            pass
        return False

    def _end_undo_step(self, undo_step_started):
        if not undo_step_started:
            return
        try:
            end_undo_step = getattr(self.song(), "end_undo_step", None)
            if end_undo_step:
                end_undo_step()
        except Exception:
            pass

    def _refresh_visible_mutator_clip(self, clip, send_notes=True):
        try:
            highlighted_clip_slot = self.song().view.highlighted_clip_slot
            if (
                highlighted_clip_slot is not None
                and highlighted_clip_slot.has_clip
                and self._live_object_identity(highlighted_clip_slot.clip) == self._live_object_identity(clip)
            ):
                self.send_selected_clip_metadata()
                if send_notes:
                    self.send_selected_clip_notes()
        except Exception:
            pass

    def _generate_mutator_clip(self, clip, settings, previous_info=None, send_updates=True, automatic=False):
        generation_key = self._live_object_identity(clip)
        now = time.time()
        with self._mutator_generation_lock:
            if generation_key in self._mutator_generation_in_progress:
                return False
            if automatic and now - self._last_mutator_generation_times.get(generation_key, 0.0) < self.MUTATOR_GENERATION_COOLDOWN_SECONDS:
                return False
            self._mutator_generation_in_progress.add(generation_key)
        try:
            if clip is None or not getattr(clip, "is_midi_clip", False):
                return False

            previous_info = previous_info or self._mutator_info(clip)
            companion_was_already_running = previous_info is not None
            decoupled_info = self._decoupled_automation_info(clip)
            source_start = decoupled_info["note_start"] if decoupled_info else float(getattr(clip, "loop_start", 0.0))
            original_loop_length = previous_info.get("original_loop_length") if previous_info else None
            if original_loop_length is None:
                if decoupled_info:
                    original_loop_length = decoupled_info["note_length"]
                else:
                    original_loop_length = max(0.0001, float(getattr(clip, "loop_end", source_start + 4.0)) - source_start)
            original_loop_length = max(0.0001, float(original_loop_length))
            source_end = source_start + original_loop_length
            source_notes = clip.get_notes_extended(0, 128, source_start, original_loop_length)
            source_values = [self._mutator_note_values(note) for note in source_notes if note.start_time >= source_start - 0.000001 and note.start_time < source_end - 0.000001]
            rhythm_mode = settings.get("companion_mode", "melody") == "rhythm"
            target_pitches = set(self._mutator_rhythm_target_pitches(settings, source_values)) if rhythm_mode else set()
            if not source_values and not target_pitches:
                return False
            settings = self._mutator_resolve_slot_activation(settings)
            generation_source_values = source_values
            passthrough_section_values = []
            if rhythm_mode:
                generation_source_values, passthrough_section_values = self._mutator_split_rhythm_source_values(
                    settings,
                    source_values,
                    source_start,
                    original_loop_length
                )

            def section_payload(values):
                return [dict(value) for value in tuple(values or ())] + [dict(value) for value in passthrough_section_values]

            roles = self._mutator_pattern_roles(settings.get("preset", 9))
            sections = []
            specs = []
            generated_section_cache = {}
            chain_preset = self._mutator_preset_is_chain(settings.get("preset", 9))
            original_section_values = self._mutator_make_section_values(
                generation_source_values,
                0,
                source_start,
                original_loop_length,
                settings,
                random.Random(0)
            )
            previous_chain_values = tuple(dict(value) for value in original_section_values)
            rnd = random.Random(int(settings.get("seed", 1)))
            for index, role in enumerate(roles):
                section_start = source_start + (index * original_loop_length)
                sections.append({"role": role, "start": section_start, "length": original_loop_length})
                preserve_source_section = index == 0

                if chain_preset:
                    if role == 0:
                        section_values = original_section_values
                        previous_chain_values = tuple(dict(value) for value in section_values)
                    elif role in generated_section_cache:
                        section_values = generated_section_cache[role]
                        previous_chain_values = tuple(dict(value) for value in section_values)
                    else:
                        section_values = self._mutator_make_section_values(
                            previous_chain_values,
                            role,
                            0.0,
                            original_loop_length,
                            settings,
                            rnd
                        )
                        generated_section_cache[role] = tuple(dict(value) for value in section_values)
                        previous_chain_values = tuple(dict(value) for value in section_values)
                    if not preserve_source_section:
                        specs.extend(self._mutator_place_section_values(section_payload(section_values), section_start, original_loop_length))
                    continue

                if role in (0, 8):
                    section_values = self._mutator_make_section_values(
                        generation_source_values,
                        role,
                        source_start,
                        original_loop_length,
                        settings,
                        rnd
                    )
                    if not preserve_source_section:
                        specs.extend(self._mutator_place_section_values(section_payload(section_values), section_start, original_loop_length))
                elif role in generated_section_cache:
                    specs.extend(self._mutator_place_section_values(section_payload(generated_section_cache[role]), section_start, original_loop_length))
                elif role == 2 and 1 in generated_section_cache:
                    section_values = self._mutator_make_section_values(
                        generated_section_cache[1],
                        role,
                        0.0,
                        original_loop_length,
                        settings,
                        rnd
                    )
                    generated_section_cache[role] = tuple(dict(value) for value in section_values)
                    specs.extend(self._mutator_place_section_values(section_payload(section_values), section_start, original_loop_length))
                elif role == 13 and 5 in generated_section_cache:
                    section_values = self._mutator_make_section_values(
                        generated_section_cache[5],
                        role,
                        0.0,
                        original_loop_length,
                        settings,
                        rnd
                    )
                    generated_section_cache[role] = tuple(dict(value) for value in section_values)
                    specs.extend(self._mutator_place_section_values(section_payload(section_values), section_start, original_loop_length))
                elif role == 12 and 6 in generated_section_cache:
                    section_values = self._mutator_make_section_values(
                        generated_section_cache[6],
                        role,
                        0.0,
                        original_loop_length,
                        settings,
                        rnd
                    )
                    generated_section_cache[role] = tuple(dict(value) for value in section_values)
                    specs.extend(self._mutator_place_section_values(section_payload(section_values), section_start, original_loop_length))
                elif role == 15 and 6 in generated_section_cache:
                    section_values = self._mutator_make_section_values(
                        generated_section_cache[6],
                        role,
                        0.0,
                        original_loop_length,
                        settings,
                        rnd
                    )
                    generated_section_cache[role] = tuple(dict(value) for value in section_values)
                    specs.extend(self._mutator_place_section_values(section_payload(section_values), section_start, original_loop_length))
                else:
                    generation_role = 5 if role == 16 else role
                    section_values = self._mutator_make_section_values(
                        generation_source_values,
                        generation_role,
                        source_start,
                        original_loop_length,
                        settings,
                        rnd
                    )
                    generated_section_cache[role] = tuple(dict(value) for value in section_values)
                    specs.extend(self._mutator_place_section_values(section_payload(section_values), section_start, original_loop_length))

            structure_length = original_loop_length * len(roles)
            previous_structure_length = max(0.0001, float(previous_info.get("structure_length", original_loop_length))) if previous_info else original_loop_length
            should_duplicate_automation = (
                not companion_was_already_running or
                structure_length > previous_structure_length + 0.000001
            )
            automation_source_length = previous_structure_length if companion_was_already_running else original_loop_length
            remove_start = source_end
            remove_end = max(
                source_start + structure_length,
                float(getattr(clip, "loop_end", 0.0)),
                float(getattr(clip, "end_marker", 0.0)),
                float(getattr(clip, "length", 0.0))
            )
            self._begin_selected_clip_update_batch()
            undo_step_started = self._begin_undo_step()
            try:
                if hasattr(clip, "remove_notes_extended"):
                    clip.remove_notes_extended(0, 128, remove_start, max(0.0001, remove_end - remove_start))
                if specs:
                    clip.add_new_notes(tuple(specs))

                clip.loop_start = source_start
                clip.start_marker = min(float(getattr(clip, "start_marker", source_start)), source_start)
                clip.loop_end = source_start + structure_length
                clip.end_marker = source_start + structure_length
                if should_duplicate_automation:
                    if decoupled_info:
                        self._couple_decoupled_automation_to_loop_length(clip, structure_length)
                    else:
                        self._duplicate_loop_automation_to_loop_length(
                            clip,
                            source_start,
                            automation_source_length,
                            structure_length
                        )
                info = self._mutator_info_from_settings(settings, {
                    "original_loop_length": original_loop_length,
                    "structure_length": structure_length,
                    "seed": settings.get("seed", 1),
                    "sections": sections,
                }, clip, commit_structure=True)
                self._save_mutator_info_to_name(clip, info)
            finally:
                self._end_undo_step(undo_step_started)
                self._end_selected_clip_update_batch()
            if send_updates:
                self._refresh_visible_mutator_clip(clip)
            self._last_mutator_generation_times[generation_key] = time.time()
            return True
        except Exception as e:
            self._debug_log("Error generating mutator clip: {}".format(str(e)))
            return False
        finally:
            with self._mutator_generation_lock:
                self._mutator_generation_in_progress.discard(generation_key)
            self._flush_queued_mutator_work(generation_key)

    def _schedule_mutator_generation(self, key, clip, settings, previous_info=None, send_updates=True, respect_cooldown=True, automatic=True):
        with self._mutator_generation_lock:
            if key in self._mutator_generation_scheduled or key in self._mutator_generation_in_progress:
                return False
            if respect_cooldown and time.time() - self._last_mutator_generation_times.get(key, 0.0) < self.MUTATOR_GENERATION_COOLDOWN_SECONDS:
                return False
            self._mutator_generation_scheduled.add(key)

        def generate_later():
            try:
                queued_work = self._pop_queued_mutator_work(key)
                generation_clip = clip
                generation_settings = settings
                generation_previous_info = previous_info
                generation_send_updates = send_updates
                generation_automatic = bool(automatic)
                apply_settings_if_generation_fails = None

                if queued_work:
                    work_type = queued_work.get("type")
                    if work_type == "generate":
                        generation_clip = queued_work.get("clip")
                        generation_settings = queued_work.get("settings", {})
                        generation_previous_info = self._mutator_info(generation_clip) or queued_work.get("previous_info")
                        generation_send_updates = bool(queued_work.get("send_updates", True))
                        generation_automatic = False
                    elif work_type == "settings":
                        generation_clip = queued_work.get("clip")
                        generation_settings = queued_work.get("settings", {})
                        generation_previous_info = self._mutator_info(generation_clip) or previous_info
                        generation_send_updates = bool(queued_work.get("send_updates", True))
                        generation_automatic = False
                        apply_settings_if_generation_fails = queued_work

                generation_succeeded = self._generate_mutator_clip(
                    generation_clip,
                    generation_settings,
                    previous_info=generation_previous_info,
                    send_updates=generation_send_updates,
                    automatic=generation_automatic
                )
                if apply_settings_if_generation_fails and not generation_succeeded:
                    self._apply_mutator_clip_settings(
                        apply_settings_if_generation_fails.get("clip"),
                        apply_settings_if_generation_fails.get("settings", {}),
                        send_updates=bool(apply_settings_if_generation_fails.get("send_updates", True)),
                        request_generation=False
                    )
                if not generation_succeeded:
                    self._mark_mutator_generation_unscheduled(key)
            finally:
                with self._mutator_generation_lock:
                    self._mutator_generation_scheduled.discard(key)
                self._flush_queued_mutator_work(key)

        try:
            self.schedule_message(1, generate_later)
            return True
        except Exception:
            with self._mutator_generation_lock:
                self._mutator_generation_scheduled.discard(key)
            return False

    def _mark_mutator_generation_unscheduled(self, key):
        with self._mutator_generation_lock:
            state = self._mutator_regeneration_states.get(key)
            if state:
                state["regenerated_for_current_cycle"] = False
            self._last_mutator_generation_signatures.pop(key, None)

    def _request_mutator_generation_after_settings_update(self, key, clip, info, previous_info, send_updates=True):
        try:
            if not self._song_is_playing():
                return False
            if int(info.get("regenerate_mode", 0)) == 0:
                return False
            if not self._mutator_generation_settings_changed(info, previous_info):
                return False
            self._mark_mutator_generation_unscheduled(key)
            settings = self._mutator_settings_from_info(info, seed=random.randint(1, 2000000000))
            if self._mutator_generation_is_busy(key):
                return self._queue_mutator_generation(key, clip, settings, previous_info=info, send_updates=send_updates)
            if self._schedule_mutator_generation(key, clip, settings, previous_info=info, send_updates=send_updates):
                return True
            self._mark_mutator_generation_unscheduled(key)
        except Exception as e:
            self._debug_log("Error requesting mutator generation after settings update: {}".format(str(e)))
        return False

    def _should_regenerate_mutator_clip(self, key, clip, info, raw_position):
        with self._mutator_generation_lock:
            if key in self._mutator_generation_scheduled or key in self._mutator_generation_in_progress:
                return False

            state = self._mutator_regeneration_states.setdefault(key, {
                "last_position": None,
                "completed_pass_count": 0,
                "regenerated_for_current_cycle": False,
            })
            now = time.time()

            cycle_start = float(getattr(clip, "loop_start", 0.0))
            cycle_end = cycle_start + max(0.0001, float(info.get("structure_length", 0.0001)))
            previous = state.get("last_position")
            wrapped_to_start = previous is not None and previous > raw_position + 0.25
            if wrapped_to_start:
                state["regenerated_for_current_cycle"] = False
                state["completed_pass_count"] = int(state.get("completed_pass_count", 0)) + 1

            regenerate_mode = int(info.get("regenerate_mode", 0))
            should_regenerate = False
            if regenerate_mode == 1:
                should_regenerate = wrapped_to_start
            elif regenerate_mode == 2:
                should_regenerate = raw_position >= cycle_end - 0.5 and raw_position < cycle_end + 0.25
            elif regenerate_mode == 3:
                should_regenerate = wrapped_to_start and int(state.get("completed_pass_count", 0)) % 2 == 0
            elif regenerate_mode == 4:
                should_regenerate = wrapped_to_start and int(state.get("completed_pass_count", 0)) % 4 == 0
            elif regenerate_mode == 5:
                should_regenerate = wrapped_to_start and random.random() < 0.10
            elif regenerate_mode == 6:
                should_regenerate = wrapped_to_start and random.random() < 0.25
            elif regenerate_mode == 7:
                should_regenerate = wrapped_to_start and random.random() < 0.50
            elif regenerate_mode == 8:
                should_regenerate = wrapped_to_start and random.random() < 0.75

            state["last_position"] = raw_position
            if should_regenerate and not state.get("regenerated_for_current_cycle", False):
                generation_signature = (
                    int(regenerate_mode),
                    int(state.get("completed_pass_count", 0)),
                    int(round(cycle_start * 1000.0)),
                    int(round(cycle_end * 1000.0))
                )
                if self._last_mutator_generation_signatures.get(key) == generation_signature:
                    return False
                if now - self._last_mutator_generation_request_times.get(key, 0.0) < self.MUTATOR_GENERATION_COOLDOWN_SECONDS:
                    return False
                self._last_mutator_generation_request_times[key] = now
                self._last_mutator_generation_signatures[key] = generation_signature
                state["regenerated_for_current_cycle"] = True
                return True
            return False

    def _should_regenerate_mutator_clip_on_launch(self, key, clip, info, raw_position):
        with self._mutator_generation_lock:
            state = self._mutator_regeneration_states.setdefault(key, {
                "last_position": None,
                "completed_pass_count": 0,
                "regenerated_for_current_cycle": False,
            })

            cycle_start = float(getattr(clip, "loop_start", 0.0))
            cycle_end = cycle_start + max(0.0001, float(info.get("structure_length", 0.0001)))
            play_count = int(state.get("completed_pass_count", 0)) + 1
            state["completed_pass_count"] = play_count
            state["last_position"] = raw_position
            state["regenerated_for_current_cycle"] = False

            regenerate_mode = int(info.get("regenerate_mode", 0))
            should_regenerate = False
            if regenerate_mode in (1, 2):
                should_regenerate = True
            elif regenerate_mode == 3:
                should_regenerate = play_count % 2 == 0
            elif regenerate_mode == 4:
                should_regenerate = play_count % 4 == 0
            elif regenerate_mode == 5:
                should_regenerate = random.random() < 0.10
            elif regenerate_mode == 6:
                should_regenerate = random.random() < 0.25
            elif regenerate_mode == 7:
                should_regenerate = random.random() < 0.50
            elif regenerate_mode == 8:
                should_regenerate = random.random() < 0.75

            if should_regenerate:
                generation_signature = (
                    "launch",
                    int(regenerate_mode),
                    int(play_count),
                    int(round(cycle_start * 1000.0)),
                    int(round(cycle_end * 1000.0))
                )
                if self._last_mutator_generation_signatures.get(key) == generation_signature:
                    return False
                self._last_mutator_generation_request_times[key] = time.time()
                self._last_mutator_generation_signatures[key] = generation_signature
                state["regenerated_for_current_cycle"] = True
                return True
            return False

    def _request_mutator_generation_for_launch(self, key, clip, info, raw_position):
        try:
            if not info or int(info.get("regenerate_mode", 0)) == 0:
                return False
            if not self._should_regenerate_mutator_clip_on_launch(key, clip, info, raw_position):
                return False
            settings = self._mutator_settings_from_info(info, seed=random.randint(1, 2000000000))
            if self._mutator_generation_is_busy(key):
                return self._queue_mutator_generation(key, clip, settings, previous_info=info, send_updates=True)
            if self._schedule_mutator_generation(
                key,
                clip,
                settings,
                previous_info=info,
                send_updates=True,
                respect_cooldown=False,
                automatic=False
            ):
                return True
            self._mark_mutator_generation_unscheduled(key)
        except Exception as e:
            self._debug_log("Error requesting mutator generation for launch: {}".format(str(e)))
        return False

    def _evaluate_mutator_regeneration(self, only_track_index=None):
        try:
            if not self._song_is_playing():
                self._mutator_regeneration_states.clear()
                self._last_mutator_generation_signatures.clear()
                self._mutator_playing_clip_keys.clear()
                self._mutator_triggered_clip_keys.clear()
                return

            active_keys = set()
            known_keys = set()
            scoped_keys = set()
            for track_index, track in enumerate(self.song().tracks):
                if only_track_index is not None and track_index != only_track_index:
                    continue
                for scene_index, clip_slot in enumerate(track.clip_slots):
                    if not clip_slot.has_clip:
                        continue
                    clip = clip_slot.clip
                    is_playing = bool(clip_slot.is_playing)
                    is_triggered = self._clip_slot_is_triggered(clip_slot)

                    if not is_playing and not is_triggered:
                        if not self._mutator_playing_clip_keys and not self._mutator_triggered_clip_keys:
                            continue
                        key = self._live_object_identity(clip)
                        if key not in self._mutator_playing_clip_keys and key not in self._mutator_triggered_clip_keys:
                            continue
                    else:
                        key = self._live_object_identity(clip)

                    known_keys.add(key)
                    scoped_keys.add(key)
                    info = self._mutator_info(clip)
                    if not info or int(info.get("regenerate_mode", 0)) == 0:
                        self._mutator_playing_clip_keys.discard(key)
                        self._mutator_triggered_clip_keys.discard(key)
                        continue

                    raw_position = float(getattr(clip, "playing_position", 0.0))

                    if is_triggered:
                        self._mutator_triggered_clip_keys.add(key)

                    if is_playing:
                        active_keys.add(key)
                        was_already_playing = key in self._mutator_playing_clip_keys
                        trigger_resolved = key in self._mutator_triggered_clip_keys and not is_triggered
                        if not was_already_playing or trigger_resolved:
                            self._request_mutator_generation_for_launch(key, clip, info, raw_position)
                            self._mutator_triggered_clip_keys.discard(key)
                        self._mutator_playing_clip_keys.add(key)
                    else:
                        self._mutator_playing_clip_keys.discard(key)
                        if not is_triggered:
                            self._mutator_triggered_clip_keys.discard(key)
                        continue

                    if self._should_regenerate_mutator_clip(key, clip, info, raw_position):
                        settings = self._mutator_settings_from_info(info, seed=random.randint(1, 2000000000))
                        if not self._schedule_mutator_generation(key, clip, settings, previous_info=info, send_updates=True):
                            self._mark_mutator_generation_unscheduled(key)

            if only_track_index is None:
                self._mutator_playing_clip_keys.intersection_update(active_keys)
                self._mutator_triggered_clip_keys.intersection_update(known_keys | active_keys)
                for key in list(self._mutator_regeneration_states.keys()):
                    if key not in active_keys:
                        self._mutator_regeneration_states.pop(key, None)
                        self._last_mutator_generation_signatures.pop(key, None)
            else:
                for key in list(self._mutator_playing_clip_keys):
                    if key in scoped_keys and key not in active_keys:
                        self._mutator_playing_clip_keys.discard(key)
        except Exception as e:
            self._debug_log("Error evaluating mutator regeneration: {}".format(str(e)))

    def _set_mutator_clip(self, message):
        try:
            settings = self._mutator_settings_from_message(message)
            clip_slot = self.song().view.highlighted_clip_slot
            if clip_slot is None or not clip_slot.has_clip:
                return
            clip = clip_slot.clip
            key = self._live_object_identity(clip)
            previous_info = self._mutator_info(clip)
            if self._mutator_generation_is_busy(key):
                self._queue_mutator_generation(key, clip, settings, previous_info=previous_info, send_updates=True)
                return
            self._generate_mutator_clip(clip, settings, previous_info=previous_info, send_updates=True)
        except Exception as e:
            self._debug_log("Error setting mutator clip: {}".format(str(e)))

    def _apply_mutator_clip_settings(self, clip, settings, send_updates=True, request_generation=True):
        if clip is None or not getattr(clip, "is_midi_clip", False):
            return False

        key = self._live_object_identity(clip)
        previous_info = self._mutator_info(clip)
        if not previous_info:
            return False

        info = self._mutator_info_from_settings(settings, previous_info, clip)
        info["pending_settings_update"] = False
        self._begin_selected_clip_update_batch()
        try:
            self._save_mutator_info_to_name(clip, info)
        finally:
            self._end_selected_clip_update_batch()
        if send_updates:
            self._refresh_visible_mutator_clip(clip, send_notes=False)
        if request_generation:
            self._request_mutator_generation_after_settings_update(key, clip, info, previous_info, send_updates=send_updates)
        return True

    def _update_mutator_clip_settings(self, message):
        try:
            settings = self._mutator_settings_from_message(message)
            clip_slot = self.song().view.highlighted_clip_slot
            if clip_slot is None or not clip_slot.has_clip:
                return
            clip = clip_slot.clip
            if not getattr(clip, "is_midi_clip", False):
                return
            key = self._live_object_identity(clip)
            if self._mutator_generation_is_busy(key):
                self._queue_mutator_settings_update(key, clip, settings, send_updates=True)
                return
            self._apply_mutator_clip_settings(clip, settings, send_updates=True)
        except Exception as e:
            self._debug_log("Error updating mutator clip settings: {}".format(str(e)))

    def _end_mutator_clip(self):
        try:
            clip_slot = self.song().view.highlighted_clip_slot
            if clip_slot is None or not clip_slot.has_clip:
                return
            clip = clip_slot.clip
            info = self._mutator_info(clip)
            if not info:
                return
            source_start = float(getattr(clip, "loop_start", 0.0))
            original_loop_length = float(info.get("original_loop_length", max(0.0001, float(getattr(clip, "loop_end", source_start)) - source_start)))
            source_end = source_start + max(0.0001, original_loop_length)
            remove_end = max(
                source_end,
                source_start + float(info.get("structure_length", original_loop_length)),
                float(getattr(clip, "loop_end", 0.0)),
                float(getattr(clip, "end_marker", 0.0)),
                float(getattr(clip, "length", 0.0))
            )
            if hasattr(clip, "remove_notes_extended"):
                clip.remove_notes_extended(0, 128, source_end, max(0.0001, remove_end - source_end))
            clip.loop_start = source_start
            clip.start_marker = min(float(getattr(clip, "start_marker", source_start)), source_start)
            clip.loop_end = source_end
            clip.end_marker = clip.loop_end
            self._remove_mutator_info_from_name(clip)
            self.send_selected_clip_metadata()
            self.send_selected_clip_notes()
        except Exception as e:
            self._debug_log("Error ending mutator clip: {}".format(str(e)))

    def _unfold_mutator_clip(self):
        try:
            clip_slot = self.song().view.highlighted_clip_slot
            if clip_slot is None or not clip_slot.has_clip:
                return
            clip = clip_slot.clip
            info = self._mutator_info(clip)
            if not info:
                return
            source_start = float(getattr(clip, "loop_start", 0.0))
            structure_length = float(info.get("structure_length", max(0.0001, float(getattr(clip, "loop_end", source_start)) - source_start)))
            clip.loop_start = source_start
            clip.start_marker = min(float(getattr(clip, "start_marker", source_start)), source_start)
            clip.loop_end = source_start + max(0.0001, structure_length)
            clip.end_marker = clip.loop_end
            self._remove_mutator_info_from_name(clip)
            self.send_selected_clip_metadata()
            self.send_selected_clip_notes()
        except Exception as e:
            self._debug_log("Error unfolding mutator clip: {}".format(str(e)))

    def _handle_tap_tempo(self):
        try:
            self.song().tap_tempo()
        except Exception:
            pass

    def _apply_decoupled_note_loop(self, clip, note_start, note_length, send_updates=True):
        try:
            if clip is None:
                return

            previous_info = self._decoupled_automation_info(clip)
            if not previous_info:
                return

            note_start = max(0.0, float(note_start))
            note_length = max(0.0001, float(note_length))
            automation_lengths = {}
            for key, length in previous_info.get("automation_lengths", {}).items():
                try:
                    length = max(0.0001, float(length))
                    if abs(length - note_length) > 0.000001:
                        automation_lengths[key] = length
                except Exception:
                    pass

            if not automation_lengths:
                folded_info = {
                    "note_start": note_start,
                    "note_length": note_length,
                    "note_end": note_start + note_length,
                    "automation_lengths": {},
                    "automation_length": note_length,
                    "physical_length": note_length,
                    "physical_end": note_start + note_length,
                    "remove_start": min(previous_info["note_start"], note_start),
                }
                self._rewrite_decoupled_note_copies(clip, folded_info)
                self._rewrite_all_decoupled_automation_envelopes(clip, folded_info)
                self._remove_decoupled_automation_info_from_name(clip)
                clip.loop_start = note_start
                clip.start_marker = min(float(getattr(clip, "start_marker", note_start)), note_start)
                clip.loop_end = note_start + note_length
                clip.end_marker = note_start + note_length
                if send_updates:
                    self.send_selected_clip_metadata()
                    self.send_selected_clip_notes()
                return

            max_physical_length = self._decoupled_automation_max_physical_length(clip, note_length)
            physical_length = self._decoupled_physical_length(note_length, automation_lengths.values(), max_physical_length)
            info = {
                "note_start": note_start,
                "note_length": note_length,
                "note_end": note_start + note_length,
                "automation_lengths": automation_lengths,
                "automation_length": note_length,
                "physical_length": physical_length,
                "physical_end": note_start + physical_length,
                "remove_start": min(previous_info["note_start"], note_start),
            }

            self._rewrite_decoupled_note_copies(clip, info)
            self._rewrite_all_decoupled_automation_envelopes(clip, info)
            clip.loop_start = info["note_start"]
            clip.start_marker = min(float(getattr(clip, "start_marker", info["note_start"])), info["note_start"])
            clip.loop_end = info["physical_end"]
            clip.end_marker = info["physical_end"]
            self._save_decoupled_automation_info_to_name(clip, info)
            if send_updates:
                self.send_selected_clip_metadata()
                self.send_selected_clip_notes()
        except Exception as e:
            self._debug_log("Error applying decoupled note loop: {}".format(str(e)))

    def _set_decoupled_automation_length(self, message):
        try:
            payload = bytes(message[2:-1]).decode('ascii', errors='ignore')
            fields = self._split_escaped_sysex_fields(payload, "|")
            if not fields:
                return

            automation_length = max(0.0001, float(fields[0]))
            control_index = max(0, min(7, int(fields[1]))) if len(fields) >= 2 and fields[1] != "" else 0
            device_param = self._current_connected_parameter_for_control(control_index)
            clip_slot = self.song().view.highlighted_clip_slot
            if clip_slot is None or not clip_slot.has_clip:
                return

            self._apply_decoupled_automation_length(clip_slot.clip, device_param, automation_length, send_updates=True)
        except Exception as e:
            self._debug_log("Error setting decoupled automation length: {}".format(str(e)))

    def _apply_decoupled_automation_length(self, clip, device_param, automation_length, send_updates=True):
        try:
            parameter_key = self._decoupled_automation_parameter_key(device_param)
            if not parameter_key:
                return

            if clip is None:
                return

            automation_length = max(0.0001, float(automation_length))
            previous_info = self._decoupled_automation_info(clip)
            if previous_info:
                note_start = previous_info["note_start"]
                note_length = previous_info["note_length"]
                automation_lengths = dict(previous_info.get("automation_lengths", {}))
            else:
                note_start = float(getattr(clip, "loop_start", 0.0))
                note_end = float(getattr(clip, "loop_end", note_start + automation_length))
                note_length = max(0.0001, note_end - note_start)
                automation_lengths = {}

            max_physical_length = self._decoupled_automation_max_physical_length(clip, note_length)
            automation_length = min(automation_length, max_physical_length)
            if abs(automation_length - note_length) <= 0.000001:
                automation_lengths.pop(parameter_key, None)
            else:
                automation_lengths[parameter_key] = automation_length

            if not automation_lengths:
                folded_info = {
                    "note_start": note_start,
                    "note_length": note_length,
                    "note_end": note_start + note_length,
                    "automation_lengths": {},
                    "automation_length": note_length,
                    "physical_length": note_length,
                    "physical_end": note_start + note_length,
                }
                self._rewrite_decoupled_note_copies(clip, folded_info)
                self._remove_decoupled_automation_info_from_name(clip)
                clip.loop_start = note_start
                clip.start_marker = min(float(getattr(clip, "start_marker", note_start)), note_start)
                clip.loop_end = note_start + note_length
                clip.end_marker = note_start + note_length
                if send_updates:
                    self.send_selected_clip_metadata()
                    self.send_selected_clip_notes()
                return

            physical_length = self._decoupled_physical_length(note_length, automation_lengths.values(), max_physical_length)
            info = {
                "note_start": note_start,
                "note_length": note_length,
                "note_end": note_start + note_length,
                "automation_lengths": automation_lengths,
                "automation_length": automation_length,
                "physical_length": physical_length,
                "physical_end": note_start + physical_length,
            }

            self._rewrite_decoupled_note_copies(clip, info)
            self._rewrite_all_decoupled_automation_envelopes(clip, info)
            if info["physical_end"] > float(getattr(clip, "loop_end", 0.0)):
                clip.loop_end = info["physical_end"]
                clip.end_marker = info["physical_end"]
            clip.loop_start = info["note_start"]
            clip.start_marker = min(float(getattr(clip, "start_marker", info["note_start"])), info["note_start"])
            clip.loop_end = info["physical_end"]
            clip.end_marker = info["physical_end"]
            self._save_decoupled_automation_info_to_name(clip, info)
            if send_updates:
                self.send_selected_clip_metadata()
                self.send_selected_clip_notes()
        except Exception as e:
            self._debug_log("Error applying decoupled automation length: {}".format(str(e)))

    def _unfold_decoupled_automation_clip(self):
        try:
            clip_slot = self.song().view.highlighted_clip_slot
            if clip_slot is None or not clip_slot.has_clip:
                return

            clip = clip_slot.clip
            info = self._decoupled_automation_info(clip)
            if not info:
                return

            clip.loop_start = info["note_start"]
            clip.start_marker = min(float(getattr(clip, "start_marker", info["note_start"])), info["note_start"])
            clip.loop_end = info["physical_end"]
            clip.end_marker = info["physical_end"]
            self._remove_decoupled_automation_info_from_name(clip)
            self.send_selected_clip_metadata()
            self.send_selected_clip_notes()
        except Exception as e:
            self._debug_log("Error unfolding decoupled automation clip: {}".format(str(e)))

    def _automation_clear_response(self, control_index, current_value):
        response = "{}|{}|{:.6f}|{}|{}".format(
            control_index,
            0,
            current_value,
            "",
            ""
        )
        self._send_sys_ex_message(response, 0x31)

    def _automation_response_decoupled_fields(self, clip, device_param):
        info = self._decoupled_automation_info(clip, device_param)
        if not info:
            return ["0", "0.000000", "0.000000"]
        return [
            "1" if info.get("has_parameter_length") else "0",
            "{:.6f}".format(info.get("automation_length", info.get("note_length", 0.0))),
            "{:.6f}".format(info.get("physical_end", info.get("note_end", 0.0))),
        ]

    def _clear_automation_envelope(self, message):
        try:
            control_index = max(0, min(7, int(message[2]) if len(message) >= 4 else 0))
            device_param = self._current_connected_parameter_for_control(control_index)
            current_value = self._parameter_normalized_value(device_param)
            clip_slot = self.song().view.highlighted_clip_slot
            if clip_slot is None or not clip_slot.has_clip or not device_param or not liveobj_valid(device_param):
                self._automation_clear_response(control_index, current_value)
                return

            clip = clip_slot.clip
            envelope = None
            if hasattr(clip, 'automation_envelope'):
                try:
                    envelope = clip.automation_envelope(device_param)
                except Exception:
                    envelope = None
            if envelope is not None:
                self._clear_clip_automation_envelope(clip, envelope, device_param, current_value)
            self._clear_authored_automation_steps_for_parameter(clip, device_param)
            self._automation_clear_response(control_index, current_value)
            self._refresh_parameter_metadata_on_automation_change()
        except Exception as e:
            self._debug_log("Error clearing automation envelope: {}".format(str(e)))

    def _clear_all_automation_envelopes(self, message):
        try:
            control_index = max(0, min(7, int(message[2]) if len(message) >= 4 else 0))
            device_param = self._current_connected_parameter_for_control(control_index)
            current_value = self._parameter_normalized_value(device_param)
            clip_slot = self.song().view.highlighted_clip_slot
            if clip_slot is None or not clip_slot.has_clip:
                self._automation_clear_response(control_index, current_value)
                return

            clip = clip_slot.clip
            for index in range(8):
                device_param = self._current_connected_parameter_for_control(index)
                if not device_param or not liveobj_valid(device_param):
                    continue
                envelope = None
                if hasattr(clip, 'automation_envelope'):
                    try:
                        envelope = clip.automation_envelope(device_param)
                    except Exception:
                        envelope = None
                if envelope is None:
                    continue
                self._clear_clip_automation_envelope(clip, envelope, device_param, self._parameter_normalized_value(device_param))
            self._clear_authored_automation_steps_for_clip(clip)
            self._automation_clear_response(control_index, current_value)
            self._refresh_parameter_metadata_on_automation_change()
        except Exception as e:
            self._debug_log("Error clearing all automation envelopes: {}".format(str(e)))

    def _send_automation_envelope(self, message):
        try:
            payload = bytes(message[2:-1]).decode('ascii', errors='ignore')
            fields = self._split_escaped_sysex_fields(payload, "|")
            if len(fields) < 4:
                return

            control_index = max(0, min(7, int(fields[0])))
            start = float(fields[1])
            step_duration = max(0.0001, float(fields[2]))
            count = max(1, min(self.AUTOMATION_ENVELOPE_MAX_SAMPLES, int(fields[3])))
            device_param = self._current_connected_parameter_for_control(control_index)
            current_value = self._parameter_normalized_value(device_param)

            clip_slot = self.song().view.highlighted_clip_slot
            clip = None
            envelope = None
            if clip_slot is not None and clip_slot.has_clip and device_param and liveobj_valid(device_param):
                clip = clip_slot.clip
                if hasattr(clip, 'automation_envelope'):
                    try:
                        envelope = clip.automation_envelope(device_param)
                    except Exception:
                        envelope = None

            samples = []
            has_envelope = 1 if envelope is not None else 0
            for index in range(count):
                time_value = start + (float(index) * step_duration)
                normalized = current_value
                if envelope is not None:
                    try:
                        raw_value = envelope.value_at_time(time_value)
                        if device_param.max != device_param.min:
                            normalized = (raw_value - device_param.min) / (device_param.max - device_param.min)
                    except Exception:
                        normalized = current_value
                normalized = max(0.0, min(1.0, normalized))
                samples.append((time_value, normalized))

            authored_steps = self._authored_automation_steps(clip, device_param, control_index)
            request_end = start + (float(count - 1) * step_duration)
            decoupled_info = self._decoupled_automation_info(clip, device_param)
            if authored_steps is not None and decoupled_info is not None:
                authored_steps = self._normalize_decoupled_logical_automation_steps(decoupled_info, authored_steps)
                self._store_authored_automation_steps(clip, device_param, control_index, authored_steps)
            authored_render_steps = None
            if authored_steps is not None and decoupled_info is not None:
                authored_render_steps = self._expanded_decoupled_automation_steps(
                    decoupled_info,
                    authored_steps,
                    step_duration
                )
            if authored_steps is not None and decoupled_info is None and not self._authored_automation_steps_match_samples(authored_steps, samples):
                points = self._compress_automation_samples(samples)
                replacement_steps = []
                for sample in points:
                    time_value, normalized = sample
                    if time_value >= start - 0.000001 and time_value <= request_end + 0.000001:
                        replacement_steps.append((time_value, step_duration, normalized, 0.0, 0, 0))
                authored_steps = self._automation_sorted_steps(
                    list(step for step in authored_steps if step[0] < start - 0.000001 or step[0] > request_end + 0.000001)
                    + replacement_steps
                )
                self._store_authored_automation_steps(clip, device_param, control_index, authored_steps)
            points = [] if authored_steps is not None else self._compress_automation_samples(samples)
            entries = []
            if authored_steps is not None:
                for step in authored_steps:
                    entries.append(self._automation_step_entry(step))
            else:
                for sample in points:
                    time_value, normalized = sample
                    if decoupled_info is not None and abs(time_value - request_end) <= 0.000001:
                        continue
                    entries.append("{:.6f}:{:.6f}:{:.6f}:{:.6f}".format(time_value, step_duration, normalized, 0.0))
            render_entries = []
            for sample in samples:
                time_value, normalized = sample
                if authored_render_steps:
                    normalized = self._automation_value_from_steps(time_value, authored_render_steps)
                render_entries.append("{:.6f}:{:.6f}:{:.6f}:{:.6f}".format(time_value, step_duration, normalized, 0.0))

            response = "{}|{}|{:.6f}|{}|{}|{}".format(
                control_index,
                has_envelope,
                current_value,
                ",".join(entries),
                ",".join(render_entries),
                "|".join(self._automation_response_decoupled_fields(clip, device_param))
            )
            self._send_sys_ex_message(response, 0x31)
        except Exception as e:
            self._debug_log("Error sending automation envelope: {}".format(str(e)))

    def _set_automation_envelope(self, message):
        try:
            payload = bytes(message[2:-1]).decode('ascii', errors='ignore')
            fields = self._split_escaped_sysex_fields(payload, "|")
            if len(fields) < 5:
                return

            control_index = max(0, min(7, int(fields[0])))
            page_start = float(fields[1])
            page_end = max(page_start, float(fields[2]))
            sample_duration = max(0.0001, float(fields[3]))
            step_entries = self._split_escaped_sysex_fields(fields[4], ",") if fields[4] else []
            write_token = fields[7] if len(fields) >= 8 else ""
            response_token_fields = [write_token] if write_token else []

            if len(fields) >= 7:
                try:
                    expected_step_count = int(fields[5])
                    expected_checksum = int(fields[6], 16)
                    checksum_payload = "|".join(fields[:5])
                    actual_checksum = self._automation_payload_checksum(checksum_payload)
                    if expected_step_count != len(step_entries) or expected_checksum != actual_checksum:
                        self._debug_log(
                            "Rejected corrupted automation envelope payload: expected {} / {:08X}, got {} / {:08X}".format(
                                expected_step_count,
                                expected_checksum,
                                len(step_entries),
                                actual_checksum
                            )
                        )
                        return
                except Exception:
                    self._debug_log("Rejected automation envelope payload with invalid checksum fields")
                    return

            device_param = self._current_connected_parameter_for_control(control_index)
            clip_slot = self.song().view.highlighted_clip_slot
            if clip_slot is None or not clip_slot.has_clip or not device_param or not liveobj_valid(device_param):
                return

            automation_was_enabled = self._parameter_automation_is_enabled(device_param)
            clip = clip_slot.clip
            envelope = None
            if hasattr(clip, 'automation_envelope'):
                try:
                    envelope = clip.automation_envelope(device_param)
                except Exception:
                    envelope = None
            automation_should_re_enable = automation_was_enabled or envelope is not None
            previous_authored_steps = self._authored_automation_steps(clip, device_param, control_index)

            current_normalized = self._parameter_normalized_value(device_param)
            try:
                clip_end = max(
                    page_end,
                    float(getattr(clip, 'loop_end', 0.0)),
                    float(getattr(clip, 'end_marker', 0.0)),
                    float(getattr(clip, 'length', 0.0))
                )
            except Exception:
                clip_end = page_end

            steps = []
            for entry in step_entries:
                components = entry.split(":")
                if len(components) < 3:
                    continue
                try:
                    time_value = max(0.0, min(clip_end, float(components[0])))
                    duration = max(0.0001, float(components[1]))
                    normalized = max(0.0, min(1.0, float(components[2])))
                    curve = max(-1.0, min(1.0, float(components[3]) if len(components) >= 4 else 0.0))
                    step_id = max(0, int(components[4])) if len(components) >= 5 else 0
                    step_order = max(0, int(components[5])) if len(components) >= 6 else step_id
                    steps.append((time_value, duration, normalized, curve, step_id, step_order))
                except Exception:
                    pass

            logical_steps = self._automation_sorted_steps(steps)
            steps = list(logical_steps)

            decoupled_info = self._decoupled_automation_info(clip, device_param)
            if decoupled_info:
                logical_steps = self._merge_decoupled_automation_span(
                    previous_authored_steps,
                    logical_steps,
                    page_start,
                    page_end
                )
                logical_steps = self._normalize_decoupled_logical_automation_steps(decoupled_info, logical_steps)
                steps = list(logical_steps)
            if decoupled_info and steps:
                steps = list(self._expanded_decoupled_automation_steps(decoupled_info, steps, sample_duration))
                clip_end = decoupled_info["physical_end"]
            response_source_steps = tuple(steps)

            def edited_span_value(time_value):
                if not steps:
                    return current_normalized

                first_step = steps[0]
                if time_value <= first_step[0]:
                    return first_step[2]

                previous_step = first_step
                for next_step in steps[1:]:
                    if abs(next_step[0] - previous_step[0]) <= 0.000001:
                        if time_value >= next_step[0]:
                            previous_step = next_step
                        continue

                    if time_value <= next_step[0]:
                        progress = max(0.0, min(1.0, (time_value - previous_step[0]) / (next_step[0] - previous_step[0])))
                        return curve_segment_value(previous_step[2], next_step[2], progress, previous_step[3])

                    previous_step = next_step

                return previous_step[2]

            def curve_segment_value(start_value, end_value, progress, curve):
                progress = max(0.0, min(1.0, progress))
                curve = max(-1.0, min(1.0, curve))
                if abs(curve) <= 0.000001:
                    return max(0.0, min(1.0, start_value + ((end_value - start_value) * progress)))

                exponent = 1.0 + (abs(curve) * 14.0)
                rises = end_value >= start_value
                bows_up = curve > 0.0
                if bows_up == rises:
                    shaped_progress = 1.0 - pow(1.0 - progress, exponent)
                else:
                    shaped_progress = pow(progress, exponent)

                blended_progress = progress + ((shaped_progress - progress) * abs(curve))
                return max(0.0, min(1.0, start_value + ((end_value - start_value) * blended_progress)))

            all_steps = []

            def append_target_step(time_value, duration, normalized, force=False):
                if time_value < -0.000001 or time_value > clip_end + 0.000001:
                    return

                time_value = max(0.0, min(clip_end, time_value))
                normalized = max(0.0, min(1.0, normalized))
                if all_steps and not force:
                    previous_time, _, previous_value, _ = all_steps[-1]
                    if abs(previous_time - time_value) <= 0.000001 and abs(previous_value - normalized) <= self.AUTOMATION_ENVELOPE_LINEAR_EPSILON:
                        return

                all_steps.append((time_value, max(0.0001, duration), normalized, bool(force)))

            def append_boundary_hold_steps():
                if not steps:
                    return

                guard_duration = 0.0001
                boundary_start = decoupled_info["note_start"] if decoupled_info else 0.0
                first_step = steps[0]
                if first_step[0] > boundary_start + guard_duration:
                    append_target_step(boundary_start, max(guard_duration, first_step[0] - boundary_start), first_step[2], force=True)

                last_step = steps[-1]
                if clip_end > last_step[0] + guard_duration:
                    append_target_step(clip_end - guard_duration, guard_duration, last_step[2], force=True)

            def append_edited_span():
                if not steps:
                    return

                if len(steps) == 1:
                    step = steps[0]
                    append_target_step(step[0], step[1], step[2])
                    return

                for index in range(len(steps) - 1):
                    start_step = steps[index]
                    next_step = steps[index + 1]
                    append_target_step(start_step[0], start_step[1], start_step[2])

                    time_value = start_step[0] + sample_duration
                    while time_value < next_step[0] - 0.000001:
                        append_target_step(time_value, sample_duration, edited_span_value(time_value))
                        time_value += sample_duration

                last_step = steps[-1]
                append_target_step(last_step[0], last_step[1], last_step[2])

            append_edited_span()
            append_boundary_hold_steps()

            if not all_steps:
                if envelope is None and hasattr(clip, 'create_automation_envelope'):
                    try:
                        envelope = clip.create_automation_envelope(device_param)
                    except Exception:
                        envelope = None
                if envelope is not None:
                    try:
                        raw_value = self._parameter_target_value_from_normalized(device_param, current_normalized)
                        boundary_start = decoupled_info["note_start"] if decoupled_info else 0.0
                        envelope.insert_step(boundary_start, max(0.0001, clip_end - boundary_start), raw_value)
                    except Exception:
                        pass
                self._clear_authored_automation_steps(clip, device_param, control_index)
                response = "{}|{}|{:.6f}|{}|{}|{}".format(
                    control_index,
                    0,
                    current_normalized,
                    "",
                    "",
                    "|".join(self._automation_response_decoupled_fields(clip, device_param) + response_token_fields)
                )
                self._send_sys_ex_message(response, 0x31)
                self._refresh_parameter_metadata_on_automation_change()
                return

            if envelope is None and hasattr(clip, 'create_automation_envelope'):
                try:
                    envelope = clip.create_automation_envelope(device_param)
                    if envelope is not None:
                        automation_should_re_enable = True
                except Exception:
                    envelope = None

            if envelope is None:
                response = "{}|{}|{:.6f}|{}|{}|{}".format(
                    control_index,
                    0,
                    current_normalized,
                    "",
                    "",
                    "|".join(self._automation_response_decoupled_fields(clip, device_param) + response_token_fields)
                )
                self._send_sys_ex_message(response, 0x31)
                self._refresh_parameter_metadata_on_automation_change()
                return

            all_steps.sort(key=lambda item: item[0])
            coalesced_steps = []
            for step in all_steps:
                if not coalesced_steps:
                    coalesced_steps.append(step)
                    continue

                previous_time, _, previous_value, previous_force = coalesced_steps[-1]
                time_value, _, normalized, force = step
                if abs(previous_time - time_value) <= 0.000001 and abs(previous_value - normalized) <= self.AUTOMATION_ENVELOPE_LINEAR_EPSILON:
                    if force and not previous_force:
                        coalesced_steps[-1] = step
                    continue
                if not force and not previous_force and abs(previous_time - time_value) > 0.000001 and abs(previous_value - normalized) <= self.AUTOMATION_ENVELOPE_LINEAR_EPSILON:
                    continue

                coalesced_steps.append(step)

            all_steps = coalesced_steps
            minimum_duration = 0.0001
            if decoupled_info and all_steps:
                self._neutralize_decoupled_automation_points(
                    envelope,
                    device_param,
                    decoupled_info,
                    previous_authored_steps,
                    all_steps[0][2],
                    sample_duration
                )
            elif all_steps:
                replace_start = max(0.0, min(clip_end, page_start))
                replace_end = max(replace_start + minimum_duration, min(clip_end, page_end))
                self._neutralize_automation_span(
                    envelope,
                    device_param,
                    replace_start,
                    replace_end,
                    edited_span_value(replace_start)
                )

            previous_insert_time = None
            for index, step in enumerate(all_steps):
                time_value, duration, normalized, _ = step
                raw_value = self._parameter_target_value_from_normalized(device_param, normalized)
                if previous_insert_time is not None and time_value <= previous_insert_time:
                    time_value = previous_insert_time + minimum_duration

                next_time = all_steps[index + 1][0] if index + 1 < len(all_steps) and all_steps[index + 1][0] > time_value else None

                if next_time is not None:
                    duration = max(minimum_duration, next_time - time_value)
                else:
                    duration = max(minimum_duration, clip_end - time_value) if clip_end > time_value else max(minimum_duration, duration)

                try:
                    envelope.insert_step(time_value, duration, raw_value)
                    previous_insert_time = time_value
                except Exception:
                    pass

            self._store_authored_automation_steps(clip, device_param, control_index, logical_steps)

            response_samples = []
            count = max(2, min(self.AUTOMATION_ENVELOPE_MAX_SAMPLES, int((page_end - page_start) / sample_duration) + 1))
            for index in range(count):
                time_value = page_start + (float(index) * sample_duration)
                normalized = current_normalized
                if response_source_steps:
                    normalized = self._automation_value_from_steps(time_value, response_source_steps)
                else:
                    try:
                        raw_value = envelope.value_at_time(time_value)
                        if device_param.max != device_param.min:
                            normalized = (raw_value - device_param.min) / (device_param.max - device_param.min)
                    except Exception:
                        normalized = current_normalized
                response_samples.append((time_value, max(0.0, min(1.0, normalized))))

            point_entries = []
            for step in logical_steps:
                point_entries.append(self._automation_step_entry(step))

            render_entries = []
            for sample in response_samples:
                time_value, normalized = sample
                render_entries.append("{:.6f}:{:.6f}:{:.6f}:{:.6f}".format(time_value, sample_duration, normalized, 0.0))

            response = "{}|{}|{:.6f}|{}|{}|{}".format(
                control_index,
                1,
                current_normalized,
                ",".join(point_entries),
                ",".join(render_entries),
                "|".join(self._automation_response_decoupled_fields(clip, device_param) + response_token_fields)
            )
            self._send_sys_ex_message(response, 0x31)
            self._re_enable_after_automation_write(device_param, automation_should_re_enable)
            self._refresh_parameter_metadata_on_automation_change()
        except Exception as e:
            self._debug_log("Error setting automation envelope: {}".format(str(e)))

    def _parameter_automation_is_enabled(self, device_param):
        try:
            return bool(device_param and liveobj_valid(device_param) and hasattr(device_param, 'automation_state') and int(device_param.automation_state) == 1)
        except Exception:
            return False

    def _re_enable_after_automation_write(self, device_param, should_re_enable=True):
        try:
            if not should_re_enable or not device_param or not liveobj_valid(device_param):
                return

            self._re_enable_parameter_automation(device_param)
            self.schedule_message(1, lambda: self._re_enable_written_automation_if_needed(device_param))
            self.schedule_message(3, lambda: self._re_enable_written_automation_if_needed(device_param))
            self._send_re_enable_automation_enabled(force=True)
        except Exception as e:
            self._debug_log("Error re-enabling written automation: {}".format(str(e)))

    def _re_enable_parameter_automation(self, device_param):
        if hasattr(device_param, 're_enable_automation'):
            device_param.re_enable_automation()
        if hasattr(self.song(), 're_enable_automation'):
            self.song().re_enable_automation()

    def _re_enable_written_automation_if_needed(self, device_param):
        try:
            if device_param and liveobj_valid(device_param) and not self._parameter_automation_is_enabled(device_param):
                self._re_enable_parameter_automation(device_param)
                self._send_re_enable_automation_enabled(force=True)
        except Exception as e:
            self._debug_log("Error retrying written automation re-enable: {}".format(str(e)))

    def _authored_automation_steps_match_samples(self, steps, samples):
        if not steps or not samples:
            return False

        close_threshold = 0.0125
        maximum_difference_threshold = 0.02
        required_close_ratio = 0.995
        max_difference = 0.0
        close_samples = 0
        for time_value, normalized in samples:
            difference = abs(self._automation_value_from_steps(time_value, steps) - normalized)
            max_difference = max(max_difference, difference)
            if difference <= close_threshold:
                close_samples += 1

        close_ratio = float(close_samples) / float(len(samples))
        return close_ratio >= required_close_ratio and max_difference <= maximum_difference_threshold

    def _automation_value_from_steps(self, time_value, steps):
        if not steps:
            return 0.0

        sorted_steps = self._automation_sorted_steps(steps)
        first_step = sorted_steps[0]
        if time_value <= first_step[0]:
            return max(0.0, min(1.0, first_step[2]))

        previous_step = first_step
        for next_step in sorted_steps[1:]:
            if abs(next_step[0] - previous_step[0]) <= 0.000001:
                if time_value >= next_step[0]:
                    previous_step = next_step
                continue

            if time_value <= next_step[0]:
                progress = max(0.0, min(1.0, (time_value - previous_step[0]) / (next_step[0] - previous_step[0])))
                return self._automation_curve_segment_value(previous_step[2], next_step[2], progress, previous_step[3] if len(previous_step) >= 4 else 0.0)

            previous_step = next_step

        return max(0.0, min(1.0, previous_step[2]))

    def _automation_curve_segment_value(self, start_value, end_value, progress, curve):
        progress = max(0.0, min(1.0, progress))
        curve = max(-1.0, min(1.0, curve))
        if abs(curve) <= 0.000001:
            return max(0.0, min(1.0, start_value + ((end_value - start_value) * progress)))

        exponent = 1.0 + (abs(curve) * 14.0)
        rises = end_value >= start_value
        bows_up = curve > 0.0
        if bows_up == rises:
            shaped_progress = 1.0 - pow(1.0 - progress, exponent)
        else:
            shaped_progress = pow(progress, exponent)

        blended_progress = progress + ((shaped_progress - progress) * abs(curve))
        return max(0.0, min(1.0, start_value + ((end_value - start_value) * blended_progress)))

    def _compress_automation_samples(self, samples):
        if len(samples) <= 2:
            return samples

        result = []
        segment = [samples[0]]
        for index in range(1, len(samples)):
            previous_sample = samples[index - 1]
            current_sample = samples[index]
            previous_flat = index >= 2 and abs(previous_sample[1] - samples[index - 2][1]) <= self.AUTOMATION_ENVELOPE_LINEAR_EPSILON
            next_flat = index + 1 < len(samples) and abs(current_sample[1] - samples[index + 1][1]) <= self.AUTOMATION_ENVELOPE_LINEAR_EPSILON
            is_jump = (
                abs(current_sample[1] - previous_sample[1]) >= self.AUTOMATION_ENVELOPE_JUMP_THRESHOLD
                and (previous_flat or next_flat)
            )

            if is_jump:
                self._append_automation_samples(result, self._simplify_automation_segment(segment))
                self._append_automation_sample(result, (current_sample[0], previous_sample[1]))
                self._append_automation_sample(result, current_sample)
                segment = [current_sample]
            else:
                segment.append(current_sample)

        self._append_automation_samples(result, self._simplify_automation_segment(segment))
        return result

    def _simplify_automation_segment(self, samples):
        if len(samples) <= 2:
            return samples

        first_time, first_value = samples[0]
        last_time, last_value = samples[-1]
        time_span = last_time - first_time
        if abs(time_span) <= 0.000001:
            return [samples[0], samples[-1]]

        max_deviation = 0.0
        split_index = 0
        for index in range(1, len(samples) - 1):
            time_value, normalized = samples[index]
            progress = (time_value - first_time) / time_span
            linear_value = first_value + ((last_value - first_value) * progress)
            deviation = abs(normalized - linear_value)
            if deviation > max_deviation:
                max_deviation = deviation
                split_index = index

        if max_deviation <= self.AUTOMATION_ENVELOPE_LINEAR_EPSILON:
            return [samples[0], samples[-1]]

        left = self._simplify_automation_segment(samples[:split_index + 1])
        right = self._simplify_automation_segment(samples[split_index:])
        return left[:-1] + right

    def _append_automation_samples(self, result, samples):
        for sample in samples:
            self._append_automation_sample(result, sample)

    def _append_automation_sample(self, result, sample):
        if not result:
            result.append(sample)
            return

        previous_time, previous_value = result[-1]
        current_time, current_value = sample
        if abs(previous_time - current_time) <= 0.000001 and abs(previous_value - current_value) <= self.AUTOMATION_ENVELOPE_LINEAR_EPSILON:
            return

        result.append(sample)

    def decode_sys_ex_scale_root(self, message):
        scale_name_bytes = message[2:-2]
        scale_name_bytes = bytes(message[2:-2])
        scale_name = self._unescape_sysex_string(scale_name_bytes.decode('utf-8'))
        root_note_index = message[-2]
        return scale_name, root_note_index

    def extract_values_from_sysex_message(self, message):
        # Extract the values from the SysEx message based on the message format
        # Replace this with your own logic to extract the desired values
        # For example, if your message is [0xF0, 0x09, value1, value2, ..., 0xF7]
        # you can extract values starting from index 2: values = message[2:-1]
        values = message[2:-1]
        return values

    def _fire_clip(self, fire, track_index, clip_index):
        track = self.song().tracks[track_index]
        clip_slot = track.clip_slots[clip_index]
        if fire == 1:
            if clip_slot.is_playing:
                clip_slot.stop()
                keys_to_clear = [
                    key for key, active in self._active_follow_actions.items()
                    if active.get("target_kind") == "clip"
                    and active.get("track_index") == track_index
                    and active.get("scene_index") == clip_index
                ]
                for key in keys_to_clear:
                    del self._active_follow_actions[key]
                if keys_to_clear:
                    self._send_follow_action_state()
            else:
                clip_slot.set_fire_button_state(1)
                self._activate_follow_action_for_clip(track_index, clip_index, clip_slot)
        else:
            clip_slot.set_fire_button_state(1)
            self._activate_follow_action_for_clip(track_index, clip_index, clip_slot)

    def _stop_track_clips(self, track_index):
        try:
            track = self.song().tracks[track_index]
        except Exception:
            return

        track.stop_all_clips()
        keys_to_clear = [
            key for key, active in self._active_follow_actions.items()
            if active.get("target_kind") == "clip"
            and active.get("track_index") == track_index
        ]
        for key in keys_to_clear:
            del self._active_follow_actions[key]
        if keys_to_clear:
            self._send_follow_action_state()

    def _delete_clip(self, track_index, clip_index):
        track = self.song().tracks[track_index]
        clip_slot = track.clip_slots[clip_index]
        self._remove_clip_follow_action_rule(track_index, clip_index)
        clip_slot.delete_clip()
        self._send_follow_action_state()

    def _duplicate_loop(self, track_index, clip_index):
        track = self.song().tracks[track_index]
        clip_slot = track.clip_slots[clip_index]
        if not clip_slot.has_clip:
            return

        clip = clip_slot.clip
        decoupled_info = self._decoupled_automation_info(clip)

        self._begin_selected_clip_update_batch()
        undo_step_started = self._begin_undo_step()

        try:
            if decoupled_info:
                folded_start = float(decoupled_info.get("note_start", 0.0))
                folded_length = max(0.0001, float(decoupled_info.get("note_length", 1.0)))
                auto_lengths = decoupled_info.get("automation_lengths", {})
                epsilon = 0.0001

                all_auto_fit = True
                if auto_lengths:
                    for auto_len in auto_lengths.values():
                        ratio = folded_length / auto_len
                        if ratio < 1.0 - epsilon or abs(ratio - round(ratio)) > epsilon:
                            all_auto_fit = False
                            break

                if all_auto_fit:
                    clip.loop_start = folded_start
                    clip.start_marker = min(float(getattr(clip, "start_marker", folded_start)), folded_start)
                    clip.loop_end = folded_start + folded_length
                    clip.end_marker = folded_start + folded_length
                    clip.duplicate_loop()
                else:
                    clip.loop_start = folded_start
                    clip.start_marker = min(float(getattr(clip, "start_marker", folded_start)), folded_start)
                    clip.loop_end = folded_start + (folded_length * 2.0)
                    clip.end_marker = folded_start + (folded_length * 2.0)

                self._remove_decoupled_automation_info_from_name(clip)
            else:
                clip.duplicate_loop()

        except Exception as e:
            self._debug_log("Error in _duplicate_loop: {}".format(str(e)))
            raise
        finally:
            self._end_undo_step(undo_step_started)
            self._end_selected_clip_update_batch()

        if decoupled_info:
            self.send_selected_clip_metadata()
            self.send_selected_clip_notes()

    def _copy_paste_clip(self, from_track, from_clip, to_track, to_clip):
        tracks = self.song().tracks

        copy_track = tracks[from_track]
        copy_clip_slot = copy_track.clip_slots[from_clip]

        paste_track = tracks[to_track]
        paste_clip_slot = paste_track.clip_slots[to_clip]

        copy_clip_slot.duplicate_clip_to(paste_clip_slot)
        self._copy_clip_follow_action_rule(from_track, from_clip, to_track, to_clip)
        self._send_follow_action_state()
        
        # set up new clip listeners
        self._register_clip_listeners()

    def _append_and_remove_clip(self, from_track, from_clip, to_track, to_clip):
        """
        Appends notes from one MIDI clip to the end of another, then removes the source clip.
        
        Args:
            from_track: Index of the track containing the clip to append
            from_clip: Index of the clip slot to append from
            to_track: Index of the track containing the destination clip
            to_clip: Index of the clip slot to append to
        """
        tracks = self.song().tracks
        source_track = tracks[from_track]
        source_clip_slot = source_track.clip_slots[from_clip]
        dest_track = tracks[to_track]
        dest_clip_slot = dest_track.clip_slots[to_clip]
        
        # Check if both clips exist and are MIDI clips
        if not source_clip_slot.has_clip or not dest_clip_slot.has_clip:
            return
        
        source_clip = source_clip_slot.clip
        dest_clip = dest_clip_slot.clip
        
        if not source_clip.is_midi_clip or not dest_clip.is_midi_clip:
            return

        _, dest_follow_rule = self._find_clip_follow_action_rule(to_track, to_clip)
        
        # Get source clip looped region only
        source_loop_start = source_clip.loop_start
        source_loop_end = source_clip.loop_end
        source_looped_length = source_loop_end - source_loop_start
        source_notes = source_clip.get_notes_extended(0, 128, source_loop_start, source_looped_length)
        
        # Get destination clip loop end position
        dest_clip_loop_end = dest_clip.loop_end
        
        # Remove any notes in destination clip that are after loop_end
        # Get ALL notes in the clip (use clip length to ensure we capture everything)
        dest_clip_full_length = max(dest_clip.loop_end, dest_clip.end_marker, dest_clip.length, 110)
        dest_clip_notes = dest_clip.get_notes_extended(0, 128, 0, dest_clip_full_length)
        
        # Collect note IDs to delete (notes at or after loop_end)
        note_ids_to_delete = []
        for note in dest_clip_notes:
            if note.start_time >= dest_clip_loop_end:
                note_ids_to_delete.append(note.note_id)
        
        # Delete all notes in one call using note IDs
        if note_ids_to_delete:
            dest_clip.remove_notes_by_id(tuple(note_ids_to_delete))
        
        # Calculate offset for appending notes (append after destination's loop_end)
        time_offset = dest_clip_loop_end - source_loop_start
        
        # Offset source notes to append at the end of destination
        if source_notes:
            new_notes = []
            for note in source_notes:
                pitch = int(note.pitch)
                start_time = note.start_time + time_offset
                duration = note.duration
                velocity = int(note.velocity)
                mute = note.mute
                probability = note.probability
                
                note_spec = MidiNoteSpecification(
                    pitch=pitch,
                    start_time=start_time,
                    duration=duration,
                    velocity=velocity,
                    mute=mute,
                    probability=probability
                )
                new_notes.append(note_spec)
            
            # Add offset notes to destination clip
            dest_clip.add_new_notes(tuple(new_notes))
            
            # Update destination clip loop_end and end_marker to include new notes
            new_end = dest_clip_loop_end + source_looped_length
            dest_clip.loop_end = new_end
            dest_clip.end_marker = new_end
        
        # Remove the source clip
        if not dest_follow_rule:
            self._copy_clip_follow_action_rule(from_track, from_clip, to_track, to_clip, remove_source=True)
        else:
            self._remove_clip_follow_action_rule(from_track, from_clip)
        source_clip_slot.delete_clip()
        self._send_follow_action_state()

    def _set_scale_root_note(self, scale, root):
        song = self.song()
        song.scale_name = scale
        song.root_note = root

        if scale.lower() == 'chromatic':
            song.scale_mode = False
        else:
            song.scale_mode = True

    def _fire_scene(self, value):
        scenes = self.song().scenes
        if value < len(scenes):
            scene = scenes[value]
            scene.fire()
            self._handled_follow_action_launches.discard(self._follow_action_key("scene", None, value))
            self._activate_follow_action_for_scene(value)

    def _select_clip_scene(self, value):
        scenes = self.song().scenes
        if value < len(scenes):
            self.song().view.selected_scene = scenes[value]
        track = self.song().view.selected_track
        if value < len(track.clip_slots):
            self.song().view.highlighted_clip_slot = track.clip_slots[value]
        self._send_selected_clip_slot(value)

    def _delete_scene(self, value):
        self.song().delete_scene(value)
        self._shift_follow_actions_after_scene_delete(value)
        self._send_follow_action_state()

    def _on_selected_scene_changed(self):
        selected_scene = self.song().view.selected_scene
        scenes_list = self.song().scenes
        new_index = self._find_track_index(selected_scene, scenes_list)
        self._send_selected_clip_slot(new_index)
        self._check_clip_playing_status(force=True)
        if self.seq_status:
            self.start_step_seq()

    def _send_selected_clip_slot(self, clip_index):
        self._send_sys_ex_message(str(clip_index), 0x10)

    def _is_instrument_device(self, device):
        try:
            if hasattr(device, 'type') and device.type == Live.Device.DeviceType.instrument:
                return True
        except Exception:
            pass

        try:
            class_name = str(device.class_name) if hasattr(device, 'class_name') else ''
            return class_name in ('InstrumentGroupDevice', 'DrumGroupDevice')
        except Exception:
            return False

    def _valid_chains(self, rack):
        if not liveobj_valid(rack) or not hasattr(rack, 'chains'):
            return []
        return [chain for chain in rack.chains if liveobj_valid(chain) and hasattr(chain, 'devices')]

    def _find_device_location(self, container, target_device, rack_context=None):
        if not liveobj_valid(container) or not hasattr(container, 'devices'):
            return None

        for index, device in enumerate(container.devices):
            if device == target_device:
                return {
                    'container': container,
                    'index': index,
                    'rack_context': rack_context
                }

            for chain in self._valid_chains(device):
                result = self._find_device_location(chain, target_device, {
                    'rack': device,
                    'rack_container': container,
                    'rack_index': index,
                    'chain': chain
                })
                if result:
                    return result

        return None

    def _rack_device_at_navigation_index(self, device_index):
        selected_track = self.song().view.selected_track
        if not selected_track or not hasattr(selected_track, 'devices'):
            return None
        
        all_devices = self._get_all_nested_devices(selected_track.devices)[0]
        live_index = self._app_device_index_to_live_index(device_index)
        if live_index < 0 or live_index >= len(all_devices):
            return None
        
        device = all_devices[live_index]
        if not isinstance(device, Live.RackDevice.RackDevice):
            return None
        return device

    def _handle_rack_snapshot_command(self, message):
        device_index = message[2]
        action = message[3]
        device = self._rack_device_at_navigation_index(device_index)
        if not liveobj_valid(device):
            return
        if action != 4:
            self._cancel_smooth_macro_randomize()
        
        try:
            if action == 0:
                previous_variation_count = self._rack_variation_count(device)
                device.store_variation()
                self._select_newly_stored_variation(device, previous_variation_count)
                self._schedule_rack_snapshot_state_refresh()
            elif action == 1:
                device.recall_last_used_variation()
            elif action == 2:
                self._randomize_rack_macros(device)
            elif action == 3 and len(message) >= 5:
                variation_index = message[4]
                if hasattr(device, 'selected_variation_index'):
                    device.selected_variation_index = variation_index
                device.recall_selected_variation()
            elif action == 4 and len(message) >= 5:
                duration = max(1.0, float(message[4]))
                self._randomize_rack_macros_smoothly(device, duration)
                return
        except Exception as e:
            self._debug_log("Rack snapshot command failed: {}".format(str(e)))
        
        self._refresh_after_rack_snapshot_action(device)

    def _rack_variation_count(self, device):
        if not liveobj_valid(device) or not hasattr(device, 'variation_count'):
            return 0
        try:
            return int(device.variation_count)
        except Exception:
            return 0

    def _select_newly_stored_variation(self, device, previous_variation_count):
        if not liveobj_valid(device):
            return
        if not hasattr(device, 'variation_count') or not hasattr(device, 'selected_variation_index'):
            return
        
        try:
            variation_count = int(device.variation_count)
            if variation_count > previous_variation_count:
                device.selected_variation_index = variation_count - 1
        except Exception as e:
            self._debug_log("Selecting stored variation failed: {}".format(str(e)))

    def _schedule_rack_snapshot_state_refresh(self):
        self.schedule_message(1, self._send_rack_snapshot_state)
        self.schedule_message(3, self._send_rack_snapshot_state)

    def _is_macro_randomize_protected_parameter(self, parameter):
        try:
            return str(parameter.name).strip().lower() == "volume"
        except Exception:
            return False

    def _macro_parameters_for_rack(self, device, protected_only=False):
        if not liveobj_valid(device) or not hasattr(device, 'parameters'):
            return []
        
        try:
            mapped_flags = list(device.macros_mapped) if hasattr(device, 'macros_mapped') else []
        except Exception:
            mapped_flags = []
        
        try:
            parameters = list(device.parameters)
        except Exception:
            parameters = []
        
        macro_parameters = []
        for macro_index, is_mapped in enumerate(mapped_flags):
            parameter_index = macro_index + 1
            if not is_mapped or parameter_index >= len(parameters):
                continue
            
            parameter = parameters[parameter_index]
            if not liveobj_valid(parameter):
                continue
            if hasattr(parameter, 'is_enabled') and not parameter.is_enabled:
                continue
            if getattr(parameter, 'max', 0) == getattr(parameter, 'min', 0):
                continue
            is_protected = self._is_macro_randomize_protected_parameter(parameter)
            if protected_only != is_protected:
                continue
            
            macro_parameters.append(parameter)
        
        return macro_parameters

    def _randomize_rack_macros(self, device):
        protected_values = [
            (parameter, parameter.value)
            for parameter in self._macro_parameters_for_rack(device, protected_only=True)
        ]
        device.randomize_macros()
        for parameter, value in protected_values:
            if not liveobj_valid(parameter):
                continue
            try:
                parameter.value = max(parameter.min, min(parameter.max, value))
            except Exception:
                pass

    def _randomize_rack_macros_smoothly(self, device, duration):
        parameters = self._macro_parameters_for_rack(device)
        if not parameters:
            self._refresh_after_rack_snapshot_action(device)
            return
        
        self._cancel_smooth_macro_randomize()
        
        starts = [parameter.value for parameter in parameters]
        try:
            self._randomize_rack_macros(device)
        except Exception as e:
            self._debug_log("Smooth macro randomize target failed: {}".format(str(e)))
            return
        
        targets = [parameter.value for parameter in parameters]
        for parameter, start in zip(parameters, starts):
            parameter.value = start
            if hasattr(parameter, 'begin_gesture'):
                try:
                    parameter.begin_gesture()
                except Exception:
                    pass

        self._smooth_macro_randomize_token += 1
        token = self._smooth_macro_randomize_token
        steps = max(1, int(duration * 10))
        self._smooth_macro_randomize_state = {
            'token': token,
            'device': device,
            'parameters': parameters,
            'starts': starts,
            'targets': targets,
            'step': 0,
            'steps': steps
        }
        self._schedule_smooth_macro_randomize_step(token)

    def _cancel_smooth_macro_randomize(self):
        self._smooth_macro_randomize_token += 1
        state = getattr(self, '_smooth_macro_randomize_state', None)
        parameters = state.get('parameters', []) if state else []
        for parameter in parameters:
            if hasattr(parameter, 'end_gesture'):
                try:
                    parameter.end_gesture()
                except Exception:
                    pass
        self._smooth_macro_randomize_state = None

    def _remove_parameter_from_smooth_macro_randomize(self, parameter):
        if not liveobj_valid(parameter):
            return
        
        state = getattr(self, '_smooth_macro_randomize_state', None)
        if not state:
            return
        
        parameters = state.get('parameters', [])
        if not any(parameter == smooth_parameter for smooth_parameter in parameters):
            return
        
        zipped_values = [
            (smooth_parameter, start, target)
            for smooth_parameter, start, target in zip(
                state.get('parameters', []),
                state.get('starts', []),
                state.get('targets', [])
            )
            if smooth_parameter != parameter
        ]
        
        if hasattr(parameter, 'end_gesture'):
            try:
                parameter.end_gesture()
            except Exception:
                pass
        
        if not zipped_values:
            self._smooth_macro_randomize_token += 1
            self._smooth_macro_randomize_state = None
            return
        
        state['parameters'] = [item[0] for item in zipped_values]
        state['starts'] = [item[1] for item in zipped_values]
        state['targets'] = [item[2] for item in zipped_values]

    def _schedule_smooth_macro_randomize_step(self, token):
        self.schedule_message(1, lambda: self._smooth_macro_randomize_step(token))

    def _smooth_macro_randomize_step(self, token):
        state = getattr(self, '_smooth_macro_randomize_state', None)
        if not state or token != self._smooth_macro_randomize_token or token != state.get('token'):
            return
        
        state['step'] += 1
        progress = min(1.0, float(state['step']) / float(state['steps']))
        eased_progress = 1.0 - pow(1.0 - progress, 3)
        
        try:
            for parameter, start, target in zip(state['parameters'], state['starts'], state['targets']):
                if not liveobj_valid(parameter):
                    continue
                value = start + ((target - start) * eased_progress)
                parameter.value = max(parameter.min, min(parameter.max, value))
        except Exception as e:
            self._debug_log("Smooth macro randomize step failed: {}".format(str(e)))
            self._cancel_smooth_macro_randomize()
            return
        
        if progress < 1.0:
            self._schedule_smooth_macro_randomize_step(token)
            return
        
        for parameter in state['parameters']:
            if hasattr(parameter, 'end_gesture'):
                try:
                    parameter.end_gesture()
                except Exception:
                    pass
        device = state.get('device')
        self._smooth_macro_randomize_state = None
        self._refresh_after_rack_snapshot_action(device)

    def _refresh_after_rack_snapshot_action(self, device):
        selected_track = self.song().view.selected_track
        selected_device = selected_track.view.selected_device if selected_track else None
        if selected_device == device:
            self._on_device_changed(False)
        self._send_rack_snapshot_state()

    def _move_device_after_device(self, source_device, target_device):
        if not liveobj_valid(source_device) or not liveobj_valid(target_device):
            return False
        if source_device == target_device or self._is_instrument_device(source_device):
            return False

        song = self.song()
        selected_track = song.view.selected_track
        source_location = self._find_device_location(selected_track, source_device)
        target_location = self._find_device_location(selected_track, target_device)
        if not source_location or not target_location:
            return False

        try:
            song.move_device(source_device, target_location['container'], target_location['index'] + 1)
            return True
        except Exception as e:
            self._debug_log("Move after failed: {}".format(str(e)))
            return False

    def _devices_changed_since(self, before_devices, after_devices):
        return [device for device in after_devices if not any(device == before for before in before_devices)]

    def _load_item_after_device(self, item, target_index):
        selected_track = self.song().view.selected_track
        before_devices = self._get_all_nested_devices(selected_track.devices)[0]
        target_index = self._app_device_index_to_live_index(target_index)
        if target_index < 0 or target_index >= len(before_devices):
            return False

        target_device = before_devices[target_index]
        self.application().browser.load_item(item)

        after_devices = self._get_all_nested_devices(selected_track.devices)[0]
        new_devices = self._devices_changed_since(before_devices, after_devices)
        selected_device = selected_track.view.selected_device if selected_track else None
        source_device = selected_device if selected_device and not any(selected_device == before for before in before_devices) else None
        if source_device is None and new_devices:
            source_device = new_devices[-1]

        if source_device:
            self._move_device_after_device(source_device, target_device)
            try:
                self.song().view.select_device(source_device)
            except Exception:
                pass
            return True

        return False

    def _set_browser_insert_after_device(self, value):
        self.browser_insert_after_device_index = value

    def _add_random_effect_after_device(self, value):
        if value is None:
            return
        selected_effect = self._random_audio_effect_item()
        if selected_effect:
            self._load_item_after_device(selected_effect, value)
            self._on_tracks_changed()
            self._on_device_changed()

    def _move_device_after_index(self, source_index, target_index):
        selected_track = self.song().view.selected_track
        all_devices = self._get_all_nested_devices(selected_track.devices)[0]
        source_index = self._app_device_index_to_live_index(source_index)
        target_index = self._app_device_index_to_live_index(target_index)
        if source_index < 0 or target_index < 0 or source_index >= len(all_devices) or target_index >= len(all_devices):
            return

        if self._move_device_after_device(all_devices[source_index], all_devices[target_index]):
            self._on_device_changed()

    def _delete_device(self, value):
        selected_track = self.song().view.selected_track
        all_devices = self._get_all_nested_devices(selected_track.devices)[0]
        value = self._app_device_index_to_live_index(value)
        if value < 0 or value >= len(all_devices):
            return
        device_to_delete = all_devices[value]

        # Try to find it in top-level devices first
        if device_to_delete in selected_track.devices:
            original_index = list(selected_track.devices).index(device_to_delete)
            selected_track.delete_device(original_index)
        else:
            # Device is nested, find its parent chain
            parent_chain, device_idx = self._find_parent_chain(selected_track.devices, device_to_delete)
            if parent_chain:
                parent_chain.delete_device(device_idx)
        self._on_device_changed()
        
        
    def _find_parent_chain(self, devices, target_device):
        """Find the parent chain and device index for a nested device"""
        for rack in devices:
            if hasattr(rack, 'chains'):
                for chain in rack.chains:
                    if target_device in chain.devices:
                        device_idx = list(chain.devices).index(target_device)
                        return chain, device_idx
                    # Recursively check nested racks
                    result = self._find_parent_chain(chain.devices, target_device)
                    if result[0]:
                        return result
        return None, None
        
    
    def _select_drum_pad(self, value):
        if self._drum_rack_device:
            drum_pad = self._drum_rack_device.drum_pads[value]
            self._drum_rack_device.view.selected_drum_pad = drum_pad

    def _move_device_left(self, value):
        song = self.song()
        selected_track = song.view.selected_track
        all_devices = self._get_all_nested_devices(selected_track.devices)[0]
        value = self._app_device_index_to_live_index(value)
        if value < 0 or value >= len(all_devices):
            return
        device_to_move = all_devices[value]
        if self._is_instrument_device(device_to_move):
            return
        location = self._find_device_location(selected_track, device_to_move)
        if not location:
            return
        moved = False
        
        # Check if it's a top-level device
        if location['container'] == selected_track:
            real_index = location['index']
            if real_index > 0:
                previous_device = list(selected_track.devices)[real_index - 1]
                if self._is_instrument_device(previous_device):
                    return
                chains = self._valid_chains(previous_device)
                if chains:
                    target_chain = chains[-1]
                    song.move_device(device_to_move, target_chain, len(target_chain.devices))
                    moved = True
                elif not self._is_instrument_device(previous_device):
                    song.move_device(device_to_move, selected_track, real_index - 1)
                    moved = True
        else:
            # Device is inside a rack, move within its chain
            parent_chain = location['container']
            device_idx = location['index']
            if device_idx > 0:
                previous_device = list(parent_chain.devices)[device_idx - 1]
                if self._is_instrument_device(previous_device):
                    return
                song.move_device(device_to_move, parent_chain, device_idx - 1)
                moved = True
        if moved:
            self._on_device_changed()
    
    def _move_device_right(self, value):
        song = self.song()
        selected_track = song.view.selected_track
        all_devices = self._get_all_nested_devices(selected_track.devices)[0]
        value = self._app_device_index_to_live_index(value)
        if value < 0 or value >= len(all_devices):
            return
        device_to_move = all_devices[value]
        if self._is_instrument_device(device_to_move):
            return
        location = self._find_device_location(selected_track, device_to_move)
        if not location:
            return
        moved = False
        
        # Check if it's a top-level device
        if location['container'] == selected_track:
            real_index = location['index']
            if real_index < len(selected_track.devices) - 1:
                song.move_device(device_to_move, selected_track, real_index + 2)
                moved = True
        else:
            # Device is inside a rack, move within its chain
            parent_chain = location['container']
            device_idx = location['index']
            if device_idx < len(parent_chain.devices) - 1:
                song.move_device(device_to_move, parent_chain, device_idx + 2)
                moved = True
            else:
                rack_context = location.get('rack_context')
                if rack_context:
                    rack_container = rack_context['rack_container']
                    rack_index = rack_context['rack_index']
                    song.move_device(device_to_move, rack_container, rack_index + 1)
                    moved = True
        if moved:
            self._on_device_changed()

    def _add_midi_track(self, value):
        song = self.song()
        song.create_midi_track(value)
        self._sync_follow_actions_to_track_topology()

    def _delete_midi_track(self, value):
        song = self.song()
        song.delete_track(value)
        self._sync_follow_actions_to_track_topology()

    def _toggle_group_fold(self, track_index):
        try:
            tracks = list(self.song().tracks)
            if track_index < 0 or track_index >= len(tracks):
                return

            group_track = self._foldable_group_track_for_track(tracks[track_index])
            if group_track is None:
                return

            group_track.fold_state = not bool(group_track.fold_state)
            self._send_group_fold_states_if_changed(tracks, force=True)
            if self.mixer_status:
                self._set_up_mixer_controls()
        except Exception:
            pass

    def _re_enable_automation(self, value):
        if value:
            try:
                self.song().re_enable_automation()
            except Exception:
                pass
            self._send_re_enable_automation_enabled(force=True)

    def _add_return_track(self, value):
        if value:
            self.song().create_return_track()
    
    def _update_mixer_status(self, value):
        if value:
            self._disconnect_device_controls()
        else:
            if hasattr(self, '_mixer_disconnect_timer') and self._mixer_disconnect_timer:
                self._mixer_disconnect_timer.cancel()
                self._mixer_disconnect_timer = None
            self.mixer_status = False
            self._set_up_mixer_controls()
            self._connect_device_controls()
            self._send_selected_device_state()

    def _update_device_status(self, value):
        if value:
            self.device_status = True
            self._check_clip_playing_status(force=True)
            if not self.mixer_status:
                self._send_selected_device_state()
        else:
            self.device_status = False
    
    def _update_step_seq(self, value):
        if value:
            self.seq_status = True
            self.start_step_seq()
        else:
           self.seq_status = False
           self.stop_step_seq()
    
    def start_step_seq(self):
        # getting the highlighted clip
        song = self.song()
        selected_clip_slot = song.view.highlighted_clip_slot
        self.send_selected_clip_metadata()
        self.send_selected_clip_notes()
        self._check_clip_playing_status(force=True)
        # self.log_message("Starting step seq")
        if self.last_selected_clip_slot is not selected_clip_slot:
            if self.last_selected_clip_slot is not None and self.last_selected_clip_slot.has_clip:
                clip = self.last_selected_clip_slot.clip
                
                if clip.notes_has_listener(self.send_selected_clip_notes):
                    # self.log_message("removing notes listener")
                    clip.remove_notes_listener(self.send_selected_clip_notes)

                if self.last_selected_clip_slot.has_clip_has_listener(self.on_highlighted_slot_changed):
                    self.last_selected_clip_slot.remove_has_clip_listener(self.on_highlighted_slot_changed)
                
                self.remove_clip_metadata_listeners(clip)
            
            # updating last selected clip
            self.last_selected_clip_slot = selected_clip_slot
            if selected_clip_slot is not None:
                if selected_clip_slot.has_clip:
                    # add notes listener
                    # self.log_message("adding notes listener")
                    if not selected_clip_slot.clip.notes_has_listener(self.send_selected_clip_notes):
                        selected_clip_slot.clip.add_notes_listener(self.send_selected_clip_notes)
                    
                    self.add_clip_metadata_listeners(selected_clip_slot.clip)
                else:
                    # create a clip slot listener that listens to clip changes
                    if not selected_clip_slot.has_clip_has_listener(self.on_highlighted_slot_changed):
                        selected_clip_slot.add_has_clip_listener(self.on_highlighted_slot_changed)
                        # self.log_message("added a has clip listener")
    
    def add_clip_metadata_listeners(self, clip):
        if not clip.end_marker_has_listener(self.send_selected_clip_metadata):
            clip.add_end_marker_listener(self.send_selected_clip_metadata)
        if not clip.start_marker_has_listener(self.send_selected_clip_metadata):
            clip.add_start_marker_listener(self.send_selected_clip_metadata)
        if not clip.loop_end_has_listener(self.send_selected_clip_metadata):
            clip.add_loop_end_listener(self.send_selected_clip_metadata)
        if not clip.loop_start_has_listener(self.send_selected_clip_metadata):
            clip.add_loop_start_listener(self.send_selected_clip_metadata)
        if not clip.signature_denominator_has_listener(self.send_selected_clip_metadata):
            clip.add_signature_denominator_listener(self.send_selected_clip_metadata)
        if not clip.signature_numerator_has_listener(self.send_selected_clip_metadata):
            clip.add_signature_numerator_listener(self.send_selected_clip_metadata)
        if hasattr(clip, "add_name_listener") and (not hasattr(clip, "name_has_listener") or not clip.name_has_listener(self.send_selected_clip_metadata)):
            clip.add_name_listener(self.send_selected_clip_metadata)
        
    def remove_clip_metadata_listeners(self, clip):
        if clip.end_marker_has_listener(self.send_selected_clip_metadata):
            clip.remove_end_marker_listener(self.send_selected_clip_metadata)
        if clip.start_marker_has_listener(self.send_selected_clip_metadata):
            clip.remove_start_marker_listener(self.send_selected_clip_metadata)
        if clip.loop_end_has_listener(self.send_selected_clip_metadata):
            clip.remove_loop_end_listener(self.send_selected_clip_metadata)
        if clip.loop_start_has_listener(self.send_selected_clip_metadata):
            clip.remove_loop_start_listener(self.send_selected_clip_metadata)
        if clip.signature_denominator_has_listener(self.send_selected_clip_metadata):
            clip.remove_signature_denominator_listener(self.send_selected_clip_metadata)
        if clip.signature_numerator_has_listener(self.send_selected_clip_metadata):
            clip.remove_signature_numerator_listener(self.send_selected_clip_metadata)
        if hasattr(clip, "remove_name_listener") and (not hasattr(clip, "name_has_listener") or clip.name_has_listener(self.send_selected_clip_metadata)):
            clip.remove_name_listener(self.send_selected_clip_metadata)
    
    def on_highlighted_slot_changed(self):
        """
        Adding note listener on a slot has clip listener slot once it gets a clip
        """
        song = self.song()
        selected_clip_slot = song.view.highlighted_clip_slot
        if selected_clip_slot.has_clip:
            self.send_selected_clip_metadata()
            self.send_selected_clip_notes()
            # add notes listener
            # self.log_message("slot now has a clip adding notes listener")
            selected_clip_slot.clip.add_notes_listener(self.send_selected_clip_notes)
            if selected_clip_slot.has_clip_has_listener(self.on_highlighted_slot_changed):
                selected_clip_slot.remove_has_clip_listener(self.on_highlighted_slot_changed)
    
    def stop_step_seq(self):
        song = self.song()
        selected_clip_slot = song.view.highlighted_clip_slot
        if selected_clip_slot is not None:
            if selected_clip_slot.has_clip_has_listener(self.on_highlighted_slot_changed):
                selected_clip_slot.remove_has_clip_listener(self.on_highlighted_slot_changed)
            if selected_clip_slot.has_clip:
                # remove notes listener
                if selected_clip_slot.has_clip_has_listener(self.on_highlighted_slot_changed):
                    selected_clip_slot.clip.remove_notes_listener(self.send_selected_clip_notes)
                # remove metadata listeners
                self.remove_clip_metadata_listeners(selected_clip_slot.clip)
            
        # reseting last selected clip
        self.last_selected_clip_slot = None
    
    # Use 7-bit encoding for multi-byte values below 1000
    def _to_2_7bit_bytes(self, value):
        return [
            value & 0x7F,           # Low 7 bits
            (value >> 7) & 0x7F     # High 7 bits
        ]
    
    def _to_3_7bit_bytes(self, value):
        """
        Convert an integer into 3 MIDI 7-bit bytes.
        Uses the highest bit of the first byte as a sign indicator.
        """
        is_negative = value < 0
        value = abs(value)
        
        if value > 0x1FFFFF:  # Cap at 2,097,151
            value = 0x1FFFFF
    
        first_byte = (value >> 14) & 0x7F
        if is_negative:
            first_byte |= 0x40  # Set the sign bit for negative numbers
    
        return [first_byte, (value >> 7) & 0x7F, value & 0x7F]
    
    def _from_3_7bit_bytes(self, bytes_list, start_index=0):
        """
        Convert 3 MIDI 7-bit bytes back into an integer.
        The highest bit of the first byte is used as a sign indicator.
        """
        if len(bytes_list) < start_index + 3:
            return 0
    
        first_byte = bytes_list[start_index]
        is_negative = (first_byte & 0x40) != 0  # Check if the sign bit is set
        value = ((first_byte & 0x3F) << 14) | (bytes_list[start_index + 1] << 7) | bytes_list[start_index + 2]
    
        return -value if is_negative else value
    
    def send_selected_clip_metadata(self):
        """
        Encode clip metadata into a compact SysEx message and send it out.
        """
        if self._selected_clip_updates_are_suppressed():
            self._selected_clip_update_pending_metadata = True
            return
        if self.seq_status:
            # self.log_message("sending clip metadata")
            status_byte = 0xF0
            end_byte = 0xF7
            manufacturer_id = 0x0E
            device_id = 0x01
            
            song = self.song()
            clip_slot = song.view.highlighted_clip_slot
            if clip_slot is not None:
                if clip_slot.has_clip:
                    selected_clip = clip_slot.clip
                    # Extract clip metadata, make ints
                    start_marker = int(selected_clip.start_marker * 1000)
                    end_marker = int(selected_clip.end_marker * 1000)
                    loop_start = int(selected_clip.loop_start * 1000)
                    loop_end = int(selected_clip.loop_end * 1000)
                    signature_denominator = int(selected_clip.signature_denominator)
                    signature_numerator = int(selected_clip.signature_numerator)
                    decoupled_info = self._decoupled_automation_info(selected_clip)
                    
                    note_data = [
                        *self._to_3_7bit_bytes(start_marker),
                        *self._to_3_7bit_bytes(end_marker),
                        *self._to_3_7bit_bytes(loop_start),
                        *self._to_3_7bit_bytes(loop_end),
                        signature_denominator,
                        signature_numerator
                    ]
                    if decoupled_info:
                        control_lengths = []
                        for control_index in range(8):
                            device_param = self._current_connected_parameter_for_control(control_index)
                            parameter_key = self._decoupled_automation_parameter_key(device_param)
                            if parameter_key in decoupled_info.get("automation_lengths", {}):
                                control_lengths.append((control_index, decoupled_info["automation_lengths"][parameter_key]))
                        note_data.extend([
                            1,
                            *self._to_3_7bit_bytes(int(decoupled_info["note_start"] * 1000)),
                            *self._to_3_7bit_bytes(int(decoupled_info["note_end"] * 1000)),
                            *self._to_3_7bit_bytes(int(decoupled_info["note_length"] * 1000)),
                            *self._to_3_7bit_bytes(int(decoupled_info["physical_end"] * 1000)),
                            len(control_lengths),
                        ])
                        for control_index, automation_length in control_lengths:
                            note_data.extend([
                                control_index,
                                *self._to_3_7bit_bytes(int(automation_length * 1000)),
                            ])
                    else:
                        note_data.append(0)

                    mutator_info = self._mutator_info(selected_clip)
                    if mutator_info:
                        note_data.extend([
                            1,
                            *self._to_3_7bit_bytes(int(mutator_info.get("original_loop_length", 0.0001) * 1000)),
                            *self._to_3_7bit_bytes(int(mutator_info.get("structure_length", 0.0001) * 1000)),
                            int(mutator_info.get("preset", 9)) & 0x7F,
                            min(32, len(mutator_info.get("sections", []))),
                        ])
                        for section in mutator_info.get("sections", [])[:32]:
                            note_data.extend([
                                int(section.get("role", 1)) & 0x7F,
                                *self._to_3_7bit_bytes(int(section.get("start", 0.0) * 1000)),
                                *self._to_3_7bit_bytes(int(section.get("length", 0.0) * 1000)),
                        ])
                        target_pitches = [max(0, min(127, int(pitch))) for pitch in mutator_info.get("target_pitches", [])[:16]]
                        operation_order = [int(slot.get("operation", 0)) for slot in self._mutator_active_slots(mutator_info)]
                        mutator_slots = self._mutator_visible_slots(mutator_info)
                        mutator_slot_count = self._mutator_slot_count_from_value(
                            mutator_info.get("mutator_slot_count", ""),
                            self._mutator_normalized_slots(mutator_info)
                        )
                        note_data.extend([
                            9,
                            int(mutator_info.get("algorithm_code", self._mutator_algorithm_code(mutator_info.get("algorithm", "mutator")))) & 0x7F,
                            int(mutator_info.get("regenerate_mode", 0)) & 0x7F,
                            int(mutator_info.get("source_mode", 2)) & 0x7F,
                            max(0, min(100, int(round(float(mutator_info.get("depth", 0.0)) * 100.0)))),
                            int(mutator_info.get("root", 0)) & 0x7F,
                            int(mutator_info.get("scale_index", 2)) & 0x7F,
                            int(mutator_info.get("settings_preset", mutator_info.get("preset", 9))) & 0x7F,
                            int(mutator_info.get("companion_mode_code", self._mutator_companion_mode_code(mutator_info.get("companion_mode", "melody")))) & 0x7F,
                            len(target_pitches),
                        ])
                        note_data.extend(target_pitches)
                        for key in self._mutator_operation_depth_keys():
                            note_data.append(max(0, min(100, int(round(self._mutator_depth_value(mutator_info, key, 0.0) * 100.0)))))
                        note_data.append(len(operation_order))
                        note_data.extend(max(0, min(self._mutator_max_operation_index(), int(index))) for index in operation_order)
                        note_data.append(len(mutator_slots))
                        for slot in mutator_slots:
                            if not slot:
                                note_data.extend([127, 0, 0, 0])
                            else:
                                operation = max(0, min(self._mutator_max_operation_index(), int(slot.get("operation", 0))))
                                note_data.extend([
                                    operation,
                                    max(0, min(100, int(round(self._mutator_depth_value(slot, "activation_probability", 1.0) * 100.0)))),
                                    max(0, min(100, int(round(self._mutator_depth_value(slot, "probability_depth", 0.0) * 100.0)))),
                                    max(0, min(100, int(round(self._mutator_depth_value(slot, "range_depth", 0.5) * 100.0)))),
                                ])
                        note_data.append(mutator_slot_count)
                        note_data.append(1 if mutator_info.get("pending_settings_update", False) else 0)
                    else:
                        note_data.append(0)
                    
                    # Send the SysEx message
                    sys_ex_message = (status_byte, manufacturer_id, device_id) + tuple(note_data) + (end_byte,)
                    self._send_midi(sys_ex_message)
    
    def send_selected_clip_notes(self):
        """
        Encode a full clip with all notes into a compact SysEx message and send it out.
        """
        if self._selected_clip_updates_are_suppressed():
            self._selected_clip_update_pending_notes = True
            return
        if self.seq_status:
            status_byte = 0xF0
            end_byte = 0xF7
            manufacturer_id = 0x0D
            device_id = 0x01
            max_chunk_length = 240
            data = bytearray()
                
            song = self.song()
            clip_slot = song.view.highlighted_clip_slot
            
            if clip_slot is not None:
                if clip_slot.has_clip:
                    selected_clip = clip_slot.clip
                
                    # Extract clip metadata
                    clip_start = min(selected_clip.start_time, selected_clip.start_marker, selected_clip.loop_start) - self.clip_length_trick
                    clip_length = (max(selected_clip.loop_end, selected_clip.end_marker, selected_clip.length) + self.clip_length_trick) - clip_start
                    
                    # Get notes
                    notes = selected_clip.get_notes_extended(0, 128, clip_start, clip_length)
                    decoupled_info = self._decoupled_automation_info(selected_clip)
                    if decoupled_info:
                        notes = [
                            note for note in notes
                            if note.start_time >= decoupled_info["note_start"] - 0.000001
                            and note.start_time < decoupled_info["note_end"] - 0.000001
                        ]
                    # self.log_message(f"Number of notes found: {len(notes)}")
                    for note in notes:
                        note_id = int(note.note_id)
                        pitch = int(note.pitch)
                        start_time = int(note.start_time * 1000)
                        duration = int(note.duration * 1000)
                        velocity = int(note.velocity)
                        mute = 1 if note.mute else 0
                        probability = int(note.probability * 127)
                    
                        note_data = [
                            *self._to_2_7bit_bytes(note_id),   # 2 bytes, 7-bit encoded
                            pitch,                      # 1 byte
                            *self._to_3_7bit_bytes(start_time),# 3 bytes, 7-bit encoded
                            *self._to_3_7bit_bytes(duration),  # 3 bytes, 7-bit encoded
                            velocity,  
                            (mute << 7) | probability
                        ]
                    
                        data.extend(note_data)
                else:
                    # Indicate no clip selected by adding a recognizable marker
                    data.extend([0x7F, 0x7F, 0x7F])
                    if not clip_slot.has_clip_has_listener(self.on_highlighted_slot_changed):
                        clip_slot.add_has_clip_listener(self.on_highlighted_slot_changed)
                
            # Split data if it's too large for a single SysEx message
            num_of_chunks = max(1, (len(data) + max_chunk_length - 1) // max_chunk_length)
                    
            for chunk_index in range(num_of_chunks):
                start_index = chunk_index * max_chunk_length
                end_index = start_index + max_chunk_length
                chunk_data = data[start_index:end_index]
                
                # Add prefix and suffix to chunks
                prefix = "_" if chunk_index == num_of_chunks - 1 else "$"
                chunk_data = prefix.encode('ascii') + chunk_data
            
                # Send the SysEx message
                sys_ex_message = (status_byte, manufacturer_id, device_id) + tuple(chunk_data) + (end_byte,)
                # self.log_message("Sending SysEx chunk")
                self._send_midi(sys_ex_message)
    
    def send_out_playing_pos(self, value, beats_per_bar, force=False, hidden=False):
        if hidden:
            cc_pair = (127, 0)
            if not force and self._last_sent_playing_pos_cc_pair == cc_pair:
                return
            self._last_sent_playing_pos_cc_pair = cc_pair
            self.send_cc(65, 11, cc_pair[0])
            self.send_cc(66, 11, cc_pair[1])
            return

        if beats_per_bar <= 0:
            return

        beats_per_bar = float(beats_per_bar)
        position = max(0.0, float(value))
        bar_index = int(math.floor(position / beats_per_bar))
        bar_position = position - (bar_index * beats_per_bar)
        fine_value = max(0.0, min(1.0, bar_position / beats_per_bar))

        cc_pair = (
            max(0, min(126, bar_index)),
            max(0, min(127, int(round(fine_value * 127.0))))
        )
        if not force and self._last_sent_playing_pos_cc_pair == cc_pair:
            return

        self._last_sent_playing_pos_cc_pair = cc_pair
        self.send_cc(65, 11, cc_pair[0])
        self.send_cc(66, 11, cc_pair[1])
            
    
    def _delete_return_track(self, value):
        song = self.song()
        song.delete_return_track(value)

    def _add_random_synth(self, value):
        if value:
            browser = self.application().browser
            # selecting an instrument from the instrument folder
            found_instrument = False
            instruments = browser.instruments
            inst_children = instruments.children

            while not found_instrument:
                random_number = random.randint(0, len(inst_children) - 1)
                rand_instrument = inst_children[random_number]
                if rand_instrument.name not in ["CV Instrument", "CV Triggers", "External Instrument", "Ext. Instrument", "Drum Rack", "Instrument Rack", "Sampler", "Simpler", "Impulse"]:
                    if rand_instrument.is_device:
                        found_instrument = True
                    else:
                        # open folder (Drum Synth)
                        children = rand_instrument.children
                        rand_index = random.randint(0, len(children) - 1)
                        rand_instrument = children[rand_index]
                        found_instrument = True

            browser.load_item(rand_instrument)
            self._on_tracks_changed()
            self._on_device_changed()

    def _add_random_drums(self, value):
        if value:
            browser = self.application().browser
            # selecting a drum rack
            drums = browser.drums.children
            number_of_drums = len(drums)
            found_drum = False
            while not found_drum:
                random_index = random.randint(0, number_of_drums - 1)
                random_drum = drums[random_index]
                if random_drum.name not in ["Drum Hits", "Drum Rack"]:
                    found_drum = True
            browser.load_item(random_drum)
            self._on_tracks_changed()
            self._on_device_changed()

    def _add_random_sound(self, value):
        if value:
            browser = self.application().browser
            # selecting a random device from the sounds folder
            sounds = browser.sounds
            number_of_sounds = len(sounds.children)
            random_index = random.randint(0, number_of_sounds - 1)
            selected_sounds_folder = sounds.children[random_index]
            number_of_sounds = len(selected_sounds_folder.children)
            random_sound_index = random.randint(0, number_of_sounds - 1)
            selected_sound = selected_sounds_folder.children[random_sound_index]
            browser.load_item(selected_sound)
            self._on_tracks_changed()
            self._on_device_changed()

    def _random_audio_effect_item(self):
        browser = self.application().browser
        # Tried loading max for live effects but gave up
        # random_index = random.randint(0, 1)
        # if random_index == 0:
        #     max_effects = browser.max_for_live.children
        #     max_number = len(max_effects)
        #     random_max = random.randint(0, max_number - 1)
        #     selected_folder = max_effects[random_max].children
        #     max_number = len(selected_folder)
        #     finished = False
        #     number_of_tries = 0
        #     if max_number == 0:
        #         selected_effect = selected_folder
        #         finished = True
        #     while not finished and number_of_tries < 10:
        #         random_max = random.randint(0, max_number - 1)
        #         selected_effect = selected_folder[random_max]
        #         number_of_tries += 1
        #         self.log_message("Selected Device: {}".format(selected_effect.name))
        #         if not any(selected_effect.name.lower().startswith(substring.lower()) for substring in ["IR", "Api", "Map8", "Max Audio Effect"]):
        #             finished = True

        # else:
        effects = browser.audio_effects
        effect_children = effects.children
        number_of_effects = len(effect_children)
        # check if effects are in folders or not
        if number_of_effects >= 10:
            random_effect_index = random.randint(0, number_of_effects - 1)
            return effect_children[random_effect_index]

        finished = False
        while not finished:
            random_folder_index = random.randint(0, number_of_effects - 1)
            selected_folder = effect_children[random_folder_index]
            self._debug_log("Selected FOlder: {}".format(selected_folder.name))
            if selected_folder.name != "Utilities":
                finished = True

        folder_children = selected_folder.children
        number_folder_children = len(folder_children)
        random_folder_child_index = random.randint(0, number_folder_children - 1)
        return folder_children[random_folder_child_index]

    def _add_random_effect(self, value):
        if value:
            selected_effect = self._random_audio_effect_item()
            if selected_effect:
                self.application().browser.load_item(selected_effect)
            self._on_tracks_changed()
            self._on_device_changed()

    def _start_browser(self, folder_index):
        """
        Start browsing a specific browser folder.

        Triggered by MIDI CC on channel 1, CC 25. The value corresponds to a folder mapping:
        0 = audio_effects, 1 = colors, 2 = current_project, 3 = drums,
        4 = instruments, 5 = max_for_live, 6 = midi_effects, 7 = packs,
        8 = plugins, 9 = sounds, 10 = user_folders, 11 = user_library

        Collects all items from the requested folder, resets pagination, and sends the first page
        via SysEx with manufacturer ID 0x13.

        Args:
            folder_index: Integer (0-11) corresponding to the folder to browse
        """
        browser = self.application().browser

        if folder_index not in self.browser_folder_mapping:
            return

        folder_name = self.browser_folder_mapping[folder_index]

        try:
            # Clear history when starting fresh
            self.browser_history = []

            # Get the browser item for the requested folder
            if folder_name in ['colors', 'user_folders']:
                # These return lists instead of BrowserItem
                folder_items = getattr(browser, folder_name)
                self.browser_current_items = list(folder_items)
            else:
                # These return a BrowserItem with children
                folder_item = getattr(browser, folder_name)
                if hasattr(folder_item, 'children'):
                    self.browser_current_items = list(folder_item.children)
                else:
                    self.browser_current_items = [folder_item]

            # Reset pagination and send first page
            self.browser_current_page = 0
            self.browser_pages_count = (len(self.browser_current_items) + self.browser_items_per_page - 1) // self.browser_items_per_page
            self._send_browser_page(self.browser_current_page)

        except Exception as e:
            self._debug_log(f"Error starting browser: {str(e)}")

    def _get_browser_item_type(self, item):
        """
        Determine the type suffix for a browser item.

        Checks the browser item's properties and returns the appropriate suffix for display
        in the app. The type can be inferred from which folder was requested.

        Args:
            item: A BrowserItem object from Live's browser

        Returns:
            String suffix: '//' for folders, '||<' for devices with children, '||' for devices without children,
            '<' for items with children that are neither folders nor devices, empty string otherwise
        """
        if hasattr(item, 'is_folder') and item.is_folder:
            return '//'
        elif hasattr(item, 'is_device') and item.is_device:
            if hasattr(item, 'children') and item.children:
                return '||<'
            else:
                return '||'
        elif hasattr(item, 'children') and item.children:
            return '<'
        else:
            return ''

    def _send_browser_page(self, page_number):
        """
        Send a paginated list of browser items via SysEx.

        Sends up to 12 items (browser_items_per_page) from the current browser folder.
        Items are formatted with type suffixes: '//' for folders, '||<' for devices with children,
        '||' for devices without children, '<' for non-folder non-device items with children.

        SysEx message format (manufacturer ID 0x13):
            current_page^total_pages^item1//,item2||<,item3||,item4<,...

        Args:
            page_number: The page number to send (0-indexed)
        """
        if page_number < 0 or page_number >= self.browser_pages_count:
            return

        self.browser_current_page = page_number

        start_index = page_number * self.browser_items_per_page
        end_index = min(start_index + self.browser_items_per_page, len(self.browser_current_items))
        page_items = self.browser_current_items[start_index:end_index]

        # Build item strings with type suffixes
        item_strings = []
        for item in page_items:
            if hasattr(item, 'name'):
                item_name = item.name
                item_type = self._get_browser_item_type(item)
                item_strings.append(f"{self._escape_sysex_string(item_name)}{item_type}")

        # Format: current_page^total_pages^item1//,item2||,item3||,...
        items_string = ','.join(item_strings)
        message = f"{self.browser_current_page}^{self.browser_pages_count}^{items_string}"

        # Send using manufacturer ID 0x13
        self._send_sys_ex_message(message, 0x13)

    def _browser_navigate(self, value):
        """
        Navigate between browser pages using a MIDI note button.

        Triggered by MIDI note on channel 15, note 82.
        - Note on (value != 0): Go to next page
        - Note off (value == 0): Go to previous page

        Validates boundaries and only sends pages that exist.

        Args:
            value: MIDI note velocity (0-127). Non-zero = note on, zero = note off
        """
        if not self.browser_current_items:
            return

        if value != 0:
            # Note on: go to next page
            if self.browser_current_page < self.browser_pages_count - 1:
                self._send_browser_page(self.browser_current_page + 1)
        else:
            # Note off: go to previous page
            if self.browser_current_page > 0:
                self._send_browser_page(self.browser_current_page - 1)

    def _browser_open_item(self, value):
        """
        Open and action on a browser item based on MIDI note velocity.

        Triggered by MIDI note on channel 15, note 81. The velocity is sent as
        (index + 1) to avoid velocity 0 issues.

        The actual item index is calculated as: (current_page * items_per_page) + (value - 1)

        Action depends on item type:
        - Folder (is_folder): Load folder contents and send new browser page
        - Has children but not a folder: Try to display children
        - Other (no children or can't display): Attempt to load the item

        After loading a device, updates track and device views to reflect changes.

        Args:
            value: MIDI note velocity (1-12), sent as index + 1
        """
        if not self.browser_current_items:
            return

        index = (self.browser_current_page * self.browser_items_per_page) + (value - 1)
        if index >= len(self.browser_current_items):
            return

        item = self.browser_current_items[index]
        browser = self.application().browser

        def load_item(item_to_load):
            target_index = self.browser_insert_after_device_index
            self.browser_insert_after_device_index = None
            if target_index is not None:
                self._load_item_after_device(item_to_load, target_index)
            else:
                browser.load_item(item_to_load)

        if hasattr(item, 'is_folder') and item.is_folder:
            if hasattr(item, 'children'):
                # Save current state to history before navigating
                self.browser_history.append({
                    'items': self.browser_current_items,
                    'page': self.browser_current_page,
                    'pages_count': self.browser_pages_count
                })
                self.browser_current_items = list(item.children)
                self.browser_current_page = 0
                self.browser_pages_count = (len(self.browser_current_items) + self.browser_items_per_page - 1) // self.browser_items_per_page
                self._send_browser_page(self.browser_current_page)
        elif hasattr(item, 'children'):
            try:
                children = list(item.children)
                if children:
                    # Save current state to history before navigating
                    self.browser_history.append({
                        'items': self.browser_current_items,
                        'page': self.browser_current_page,
                        'pages_count': self.browser_pages_count
                    })
                    self.browser_current_items = children
                    self.browser_current_page = 0
                    self.browser_pages_count = (len(self.browser_current_items) + self.browser_items_per_page - 1) // self.browser_items_per_page
                    self._send_browser_page(self.browser_current_page)
                else:
                    load_item(item)
                    self._on_tracks_changed()
                    self._on_device_changed()
            except Exception:
                load_item(item)
                self._on_tracks_changed()
                self._on_device_changed()
        else:
            load_item(item)
            self._on_tracks_changed()
            self._on_device_changed()

    def _browser_load_item(self, value):
        """
        Load a browser item based on MIDI note velocity, never opening children.

        Triggered by MIDI note on channel 15, note 80. The velocity is sent as
        (index + 1) to avoid velocity 0 issues.

        The actual item index is calculated as: (current_page * items_per_page) + (value - 1)

        Always attempts to load the item, regardless of whether it's a folder, device with children, etc.
        After loading, updates track and device views to reflect changes.

        Args:
            value: MIDI note velocity (1-12), sent as index + 1
        """
        if not self.browser_current_items:
            return

        index = (self.browser_current_page * self.browser_items_per_page) + (value - 1)
        if index >= len(self.browser_current_items):
            return

        item = self.browser_current_items[index]
        browser = self.application().browser
        target_index = self.browser_insert_after_device_index
        self.browser_insert_after_device_index = None
        if target_index is not None:
            self._load_item_after_device(item, target_index)
        else:
            browser.load_item(item)
        self._on_tracks_changed()
        self._on_device_changed()

    def _browser_go_back(self, value):
        """
        Go back one or more levels in the browser history, restoring the page state.

        Triggered by MIDI note on channel 15, note 79.

        The velocity determines how many steps to go back:
        - Velocity 1 = go back 1 step
        - Velocity 2 = go back 2 steps
        - etc.

        Restores the previous level's items and page number from the history stack.
        Only works if there's history available.

        Args:
            value: MIDI note velocity (1-127). Determines number of steps to go back.
        """
        if value == 0 or not self.browser_history:
            return

        # Determine number of steps to go back
        steps = min(value, len(self.browser_history))

        # Pop the last state(s) from history
        for _ in range(steps - 1):
            self.browser_history.pop()

        previous_state = self.browser_history.pop()

        # Restore the previous state
        self.browser_current_items = previous_state['items']
        self.browser_current_page = previous_state['page']
        self.browser_pages_count = previous_state['pages_count']

        # Send the page we were at
        self._send_browser_page(self.browser_current_page)

    def disconnect(self):
        # Cancel all pending timers
        if self._metadata_recheck_timer:
            self._metadata_recheck_timer.cancel()
            self._metadata_recheck_timer = None
        if self._automation_metadata_update_timer:
            self._automation_metadata_update_timer.cancel()
            self._automation_metadata_update_timer = None
        if hasattr(self, '_mixer_disconnect_timer') and self._mixer_disconnect_timer:
            self._mixer_disconnect_timer.cancel()
            self._mixer_disconnect_timer = None
        self._cancel_bank_metadata_refreshes()
        self._cancel_smooth_macro_randomize()
        if hasattr(self, '_periodic_timer_ref') and self._periodic_timer_ref:
            self._periodic_timer_ref.cancel()
            self._periodic_timer_ref = None
        self._remove_follow_action_runtime_listeners()
        self._remove_follow_action_name_listeners()
        self._remove_follow_action_song_listeners()
        self._mutator_regeneration_states.clear()
        self._mutator_generation_in_progress.clear()
        self._mutator_generation_scheduled.clear()
        self._queued_mutator_work.clear()
        self._last_mutator_generation_times.clear()
        self._last_mutator_generation_request_times.clear()
        self._last_mutator_generation_signatures.clear()
        
        # Stop periodic execution
        self.periodic_timer = 0
        
        # Clear caches
        self._metadata_cache.clear()
        self._metadata_send_seq_by_device.clear()
        for control_index in list(getattr(self, '_active_high_resolution_gestures', set())):
            mapped_parameter = self._mapped_parameter_for_device_control(control_index)
            if mapped_parameter and hasattr(mapped_parameter, 'end_gesture'):
                try:
                    mapped_parameter.end_gesture()
                except Exception:
                    pass
        self._active_high_resolution_gestures.clear()
        for control_index in list(getattr(self, '_active_high_resolution_undo_steps', set())):
            self._end_high_resolution_undo_step(control_index)
        
        self._remove_parameter_value_listeners()
        self._remove_parameter_name_listeners()
        self._remove_parameter_source_listener()
        self._remove_disabled_parameter_listeners()
        self._remove_automation_state_listeners()
        self._remove_mixer_automation_state_listeners()
        self._cancel_mixer_automation_status_resends()
#        self.quantize_button.remove_value_listener(self._quantize_button_value)
        if hasattr(self, 'duplicate_button'):
            self.duplicate_button.remove_value_listener(self._duplicate_button_value)
        if hasattr(self, 'duplicate_scene_button'):
            self.duplicate_scene_button.remove_value_listener(self._duplicate_scene_button_value)
        if hasattr(self, 'sesh_record_button'):
            self.sesh_record_button.remove_value_listener(self._sesh_record_value)
        if hasattr(self, 'redo_button'):
            self.redo_button.remove_value_listener(self._redo_button_value)
        if hasattr(self, 'undo_button'):
            self.undo_button.remove_value_listener(self._undo_button_value)
        if hasattr(self, 'transport_toggle_button'):
            self.transport_toggle_button.remove_value_listener(self._transport_toggle_value)
        # browser buttons cleanup
        if hasattr(self, 'browser_start_button'):
            self.browser_start_button.remove_value_listener(self._start_browser)
        if hasattr(self, 'browser_navigate_button'):
            self.browser_navigate_button.remove_value_listener(self._browser_navigate)
        if hasattr(self, 'browser_open_button'):
            self.browser_open_button.remove_value_listener(self._browser_open_item)
        if hasattr(self, 'browser_load_button'):
            self.browser_load_button.remove_value_listener(self._browser_load_item)
        if hasattr(self, 'browser_back_button'):
            self.browser_back_button.remove_value_listener(self._browser_go_back)
        if hasattr(self, 're_enable_automation_button'):
            self.re_enable_automation_button.remove_value_listener(self._re_enable_automation)
        if hasattr(self, 'remove_automation_button'):
            self.remove_automation_button.remove_value_listener(self._arm_remove_automation_from_next_encoder)
        if hasattr(self, 're_enable_parameter_automation_button'):
            self.re_enable_parameter_automation_button.remove_value_listener(self._arm_re_enable_automation_from_next_encoder)
        song = self.song()
        # periodic_check_button.remove_value_listener(self._periodic_check)
        song.remove_tracks_listener(self._on_tracks_changed)
        # self.song().view.remove_selected_track_listener(self._on_selected_track_changed)
        # self._unregister_clip_and_audio_listeners()
        # self.remove_midi_listener(self._midi_listener)
        # self.song().view.remove_selected_scene_listener(self._on_selected_scene_changed)
        song.remove_scale_name_listener(self._on_scale_changed)
        song.remove_root_note_listener(self._on_scale_changed)
        self._remove_song_listener(song, "metronome", self._update_metronome)
        self._remove_song_listener(song, "session_record", self._on_session_record_changed)
        self._remove_song_listener(song, "re_enable_automation_enabled", self._on_re_enable_automation_enabled_changed)
        try:
            if hasattr(song, 'remove_is_playing_listener') and (
                not hasattr(song, 'is_playing_has_listener')
                or song.is_playing_has_listener(self._on_song_is_playing_changed)
            ):
                song.remove_is_playing_listener(self._on_song_is_playing_changed)
        except Exception:
            pass
        # Clean up level listeners
        for track, (left_listener, right_listener) in self._track_level_listeners.items():
            if liveobj_valid(track) and hasattr(track, 'output_meter_left_has_listener') and track.output_meter_left_has_listener(left_listener):
                track.remove_output_meter_left_listener(left_listener)
            if liveobj_valid(track) and hasattr(track, 'output_meter_right_has_listener') and track.output_meter_right_has_listener(right_listener):
                track.remove_output_meter_right_listener(right_listener)

        for track, (left_listener, right_listener) in self._return_level_listeners.items():
            if liveobj_valid(track) and hasattr(track, 'output_meter_left_has_listener') and track.output_meter_left_has_listener(left_listener):
                track.remove_output_meter_left_listener(left_listener)
            if liveobj_valid(track) and hasattr(track, 'output_meter_right_has_listener') and track.output_meter_right_has_listener(right_listener):
                track.remove_output_meter_right_listener(right_listener)

        if self._master_level_listeners:
            master_track = self.song().master_track
            left_listener, right_listener = self._master_level_listeners.get(master_track, (None, None))
            if left_listener and liveobj_valid(master_track) and hasattr(master_track, 'output_meter_left_has_listener') and master_track.output_meter_left_has_listener(left_listener):
                master_track.remove_output_meter_left_listener(left_listener)
            if right_listener and liveobj_valid(master_track) and hasattr(master_track, 'output_meter_right_has_listener') and master_track.output_meter_right_has_listener(right_listener):
                master_track.remove_output_meter_right_listener(right_listener)
        
        super(Tap, self).disconnect()
