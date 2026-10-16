"""Track Properties, DYNAMIC_GROUPS and new-group requests on the send side.

SendTrackConfig carries the track's Track Properties (opaque bytes) and the
dynamic_groups declaration to the native track configuration exactly as
given; Sender.take_new_group_request reports the service's pending request
for a track once and then None. The fixture stands in for the service, so
these rows prove the binding's mapping, not the service's own behaviour
(service/tests/test_media_sender_track_status.c covers that).
"""

import gc
import unittest

import moq5
from moq5 import _native

import test_foundation as foundation  # noqa: F401

NS = (b"svc", b"demo")


class SendNewGroupTests(unittest.TestCase):
    def setUp(self):
        gc.collect()
        _native._test_reset()
        _native._test_sender_reset()
        _native._test_send_track_reset()

    def video(self, **fields):
        base = dict(name=b"v", media_type=moq5.MediaType.VIDEO,
                    packaging=moq5.Packaging.RAW, codec=b"av01", bitrate=1_000_000)
        base.update(fields)
        return moq5.SendTrackConfig(**base)

    def attach(self, endpoint):
        return moq5.Sender.attach(
            endpoint,
            moq5.SenderConfig(namespace=NS,
                              backpressure=moq5.Backpressure.DROP_GROUP))

    def connect(self):
        return moq5.Endpoint.connect(moq5.EndpointConfig(url="moqt://fixture.invalid"))

    def test_a_properties_and_dynamic_groups_reach_the_native_config(self):
        with self.connect() as endpoint:
            sender = self.attach(endpoint)
            try:
                sender.add_track(self.video(track_properties=b"\x3c\x01\x00",
                                            dynamic_groups=True))
                cfg = _native._test_send_track_config()
            finally:
                sender.close()
        self.assertEqual(cfg["track_properties"], b"\x3c\x01\x00")
        self.assertIs(cfg["dynamic_groups"], True)
        self.assertIs(cfg["full_size"], True)

    def test_b_absent_by_default(self):
        with self.connect() as endpoint:
            sender = self.attach(endpoint)
            try:
                sender.add_track(self.video())
                cfg = _native._test_send_track_config()
            finally:
                sender.close()
        self.assertEqual(cfg["track_properties"], b"")
        self.assertIs(cfg["dynamic_groups"], False)

    def test_c_a_pending_request_is_taken_once(self):
        with self.connect() as endpoint:
            sender = self.attach(endpoint)
            try:
                track = sender.add_track(self.video(dynamic_groups=True))
                self.assertIsNone(sender.take_new_group_request(track))
                _native._test_send_track_new_group(0, 7)
                self.assertEqual(sender.take_new_group_request(track), 7)
                self.assertIsNone(sender.take_new_group_request(track))
                _native._test_send_track_new_group(0, 0)
                self.assertEqual(sender.take_new_group_request(track), 0)
            finally:
                sender.close()

    def test_d_types_are_checked_before_any_native_call(self):
        with self.assertRaises(TypeError):
            self.video(track_properties=bytearray(b"\x3c\x01"))
        with self.assertRaises(TypeError):
            self.video(dynamic_groups=1)


if __name__ == "__main__":
    unittest.main()
