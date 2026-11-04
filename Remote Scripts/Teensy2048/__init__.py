from __future__ import absolute_import, print_function, unicode_literals

from .Teensy2048 import Teensy2048


def create_instance(c_instance):
    return Teensy2048(c_instance)
