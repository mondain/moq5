"""Sized event mapping only; causal native coverage is in the service suite."""

import dataclasses
import unittest

import moq5
from moq5 import _native


class ReceiverDeliveryFailureTests(unittest.TestCase):
    def setUp(self):
        _native._test_reset()
        self.endpoint = moq5.Endpoint.connect(moq5.EndpointConfig(url="moqt://fixture.invalid"))
        self.receiver = moq5.Receiver.attach(self.endpoint, moq5.ReceiverConfig.live((b"live",)))
        self.capsule = _native._test_new_track(self.receiver._Receiver__handle)
        _native._test_track_desc(self.capsule, {"name": b"video", "packaging": 1})
        self.layout = _native._test_layout()

    def tearDown(self):
        self.receiver.close()
        self.endpoint.close()

    def event(self, origin, request_id, *, stamp=None, kind=None, track=True):
        c = _native.constants
        _native._test_push_event(
            c["MEDIA_TRACK_DELIVERY_LOCAL_FAILURE"] if kind is None else kind,
            self.capsule if track else None, stamp, None, None, None,
            c["ERR_BUFFER"], (123, c["FILL_COMPLETE"], c["FILL_ERROR_NONE"], 0),
            (origin, request_id))
        return self.receiver.poll_track()

    def test_origins_zero_id_and_kind_isolation(self):
        c = _native.constants
        for origin in [c["MEDIA_DELIVERY_LIVE"], c["MEDIA_DELIVERY_FILL"], 0xFFFFFFFF]:
            for request_id in [0, (1 << 64) - 1]:
                event = self.event(origin, request_id)
                self.assertEqual(event.kind_extended, moq5.ExtendedTrackEventKind.DELIVERY_LOCAL_FAILURE)
                self.assertEqual(event.local_result, c["ERR_BUFFER"])
                self.assertEqual(event.delivery_origin, origin)
                self.assertEqual(event.delivery_initiating_request_id,
                                 request_id if origin == c["MEDIA_DELIVERY_FILL"] else None)
                self.assertIsNone(event.fill_terminal)
                self.assertEqual(event, dataclasses.replace(event, delivery_origin=None,
                                                           delivery_initiating_request_id=None))
        event = self.event(c["MEDIA_DELIVERY_FILL"], 0, kind=c["MEDIA_TRACK_FILL_TERMINAL"])
        self.assertEqual(event.fill_terminal.initiating_request_id, 123)
        self.assertIsNone(event.local_result)
        self.assertIsNone(event.delivery_origin)
        self.assertIsNone(event.delivery_initiating_request_id)

    def test_each_field_requires_its_complete_prefix(self):
        c = _native.constants
        result_end = self.layout["event_local_result_end"]
        origin_end = self.layout["event_delivery_origin_end"]
        id_end = self.layout["event_delivery_initiating_request_id_end"]
        for stamp in range(self.layout["event_v0"], self.layout["event_sizeof"] + 1):
            event = self.event(c["MEDIA_DELIVERY_FILL"], 0, stamp=stamp)
            self.assertEqual(event.local_result, c["ERR_BUFFER"] if stamp >= result_end else None)
            self.assertEqual(event.delivery_origin, moq5.DeliveryOrigin.FILL if stamp >= origin_end else None)
            self.assertEqual(event.delivery_initiating_request_id, 0 if stamp >= id_end else None)

    def test_delivery_failure_requires_a_track(self):
        with self.assertRaises(moq5.EventLost):
            self.event(_native.constants["MEDIA_DELIVERY_FILL"], 0, track=False)
