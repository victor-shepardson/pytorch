# Copyright (c) Facebook, Inc. and its affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import os
import socket
from datetime import timedelta
from typing import Optional
from unittest import TestCase

from torch.distributed import Store
from torch.distributed.elastic.rendezvous import RendezvousParameters
from torch.distributed.elastic.rendezvous.dynamic_rendezvous import (
    DynamicRendezvousHandler,
    RendezvousBackend,
    RendezvousTimeout,
    _NodeDesc,
    _NodeDescGenerator,
    create_handler,
)


class RendezvousTimeoutTest(TestCase):
    def test_init_initializes_timeout(self) -> None:
        timeout = RendezvousTimeout(
            timedelta(seconds=50),
            timedelta(seconds=60),
            timedelta(seconds=70),
        )

        self.assertEqual(timeout.join, timedelta(seconds=50))
        self.assertEqual(timeout.last_call, timedelta(seconds=60))
        self.assertEqual(timeout.close, timedelta(seconds=70))

    def test_init_initializes_timeout_if_no_timeout_is_specified(self) -> None:
        timeout = RendezvousTimeout()

        self.assertEqual(timeout.join, timedelta(seconds=600))
        self.assertEqual(timeout.last_call, timedelta(seconds=30))
        self.assertEqual(timeout.close, timedelta(seconds=30))

    def test_init_raises_error_if_timeout_is_not_positive(self) -> None:
        join_timeouts = [timedelta(seconds=0), timedelta(seconds=-1)]

        for join_timeout in join_timeouts:
            with self.subTest(join_timeout=join_timeout):
                with self.assertRaisesRegex(
                    ValueError, rf"^The join timeout \({join_timeout}\) must be positive.$"
                ):
                    timeout = RendezvousTimeout(join_timeout)


class NodeDescTest(TestCase):
    def test_repr(self) -> None:
        desc = _NodeDesc("dummy_fqdn", 3, 5)

        self.assertEqual(repr(desc), "dummy_fqdn_3_5")

    def test_hash(self) -> None:
        desc1 = _NodeDesc("dummy_fqdn", 2, 4)
        desc2 = _NodeDesc("dummy_fqdn", 3, 5)

        descs = {desc1, desc2}

        self.assertIn(desc1, descs)
        self.assertIn(desc2, descs)


class NodeDescGeneratorTest(TestCase):
    def test_generate(self) -> None:
        desc_generator = _NodeDescGenerator()

        fqdn = socket.getfqdn()

        pid = os.getpid()

        for local_id in range(4):
            with self.subTest(fqdn=fqdn, pid=pid, local_id=local_id):
                desc = desc_generator.generate()

                self.assertEqual(repr(desc), f"{fqdn}_{pid}_{local_id}")


class DummyStore(Store):
    pass


class DummyRendezvousBackend(RendezvousBackend):
    @property
    def name(self):
        return "dummy_backend"

    def get_state(self):
        return None

    def set_state(self, state, token):
        return None


class DynamicRendezvousHandlerTest(TestCase):
    def setUp(self) -> None:
        self._run_id = "dummy_run_id"
        self._store = DummyStore()
        self._backend = DummyRendezvousBackend()
        self._min_nodes = 3
        self._max_nodes = 6
        self._timeout: Optional[RendezvousTimeout] = RendezvousTimeout()

    def _create_handler(self) -> DynamicRendezvousHandler:
        return DynamicRendezvousHandler(
            run_id=self._run_id,
            store=self._store,
            backend=self._backend,
            min_nodes=self._min_nodes,
            max_nodes=self._max_nodes,
            timeout=self._timeout,
        )

    def test_init_initializes_handler(self) -> None:
        handler = self._create_handler()

        self.assertIs(handler.store, self._store)
        self.assertIs(handler.backend, self._backend)

        self.assertEqual(handler.get_backend(), self._backend.name)
        self.assertEqual(handler.get_run_id(), self._run_id)
        self.assertEqual(handler.run_id, self._run_id)
        self.assertEqual(handler.min_nodes, self._min_nodes)
        self.assertEqual(handler.max_nodes, self._max_nodes)

        if self._timeout is None:
            self.assertIsNotNone(handler.timeout)
        else:
            self.assertIs(handler.timeout, self._timeout)

    def test_init_initializes_handler_if_timeout_is_not_specified(self) -> None:
        self._timeout = None

        self.test_init_initializes_handler()

    def test_init_initializes_handler_if_min_and_max_nodes_are_equal(self) -> None:
        self._min_nodes = 3
        self._max_nodes = 3

        self.test_init_initializes_handler()

    def test_init_raises_error_if_min_nodes_is_not_positive(self) -> None:
        for num in [0, -10]:
            with self.subTest(min_nodes=num):
                self._min_nodes = num

                with self.assertRaisesRegex(
                    ValueError,
                    rf"^The minimum number of nodes \({num}\) must be greater than zero.$",
                ):
                    self._create_handler()

    def test_init_raises_error_if_max_nodes_is_less_than_min(self) -> None:
        self._min_nodes = 3
        self._max_nodes = 2

        with self.assertRaisesRegex(
            ValueError,
            rf"^The maximum number of nodes \({self._max_nodes}\) must be greater than or equal to "
            "the minimum number of nodes "
            rf"\({self._min_nodes}\).$",
        ):
            self._create_handler()


class CreateHandlerTest(TestCase):
    def setUp(self) -> None:
        self._store = DummyStore()

        self._backend = DummyRendezvousBackend()

        self._params = RendezvousParameters(
            backend=self._backend.name,
            endpoint="dummy_endpoint",
            run_id="dummy_run_id",
            min_nodes=3,
            max_nodes=6,
            store_port="1234",
            join_timeout="50",
            last_call_timeout="60",
            close_timeout="70",
        )

        self._expected_timeout = RendezvousTimeout(
            timedelta(seconds=50), timedelta(seconds=60), timedelta(seconds=70)
        )

    def test_create_handler_returns_handler(self) -> None:
        handler = create_handler(self._store, self._backend, self._params)

        self.assertIs(handler.store, self._store)
        self.assertIs(handler.backend, self._backend)

        self.assertEqual(handler.get_backend(), self._backend.name)
        self.assertEqual(handler.get_run_id(), self._params.run_id)
        self.assertEqual(handler.min_nodes, self._params.min_nodes)
        self.assertEqual(handler.max_nodes, self._params.max_nodes)
        self.assertEqual(handler.timeout.join, self._expected_timeout.join)
        self.assertEqual(handler.timeout.last_call, self._expected_timeout.last_call)
        self.assertEqual(handler.timeout.close, self._expected_timeout.close)

    def test_create_handler_returns_handler_if_timeout_is_not_specified(self) -> None:
        del self._params.config["join_timeout"]
        del self._params.config["last_call_timeout"]
        del self._params.config["close_timeout"]

        self._expected_timeout = RendezvousTimeout()

        self.test_create_handler_returns_handler()
