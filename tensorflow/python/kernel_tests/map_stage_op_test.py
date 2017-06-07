# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import data_flow_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.platform import test


class MapStageTest(test.TestCase):

  def testSimple(self):
    with self.test_session(use_gpu=True) as sess:
      with ops.device('/cpu:0'):
        x = array_ops.placeholder(dtypes.float32)
        pi = array_ops.placeholder(dtypes.int64)
        gi = array_ops.placeholder(dtypes.int64)
        v = 2. * (array_ops.zeros([128, 128]) + x)
      with ops.device(test.gpu_device_name()):
        stager = data_flow_ops.MapStagingArea([dtypes.float32])
        stage = stager.put(pi, [v], [0])
        k, y = stager.get(gi)
        y = math_ops.reduce_max(math_ops.matmul(y, y))
      sess.run(stage, feed_dict={x: -1, pi: 0})
      for i in range(10):
        _, yval = sess.run([stage, y], feed_dict={x: i, pi: i+1, gi:i})
        self.assertAllClose(4 * (i - 1) * (i - 1) * 128, yval, rtol=1e-4)

  def testMultiple(self):
    with self.test_session(use_gpu=True) as sess:
      with ops.device('/cpu:0'):
        x = array_ops.placeholder(dtypes.float32)
        pi = array_ops.placeholder(dtypes.int64)
        gi = array_ops.placeholder(dtypes.int64)
        v = 2. * (array_ops.zeros([128, 128]) + x)
      with ops.device(test.gpu_device_name()):
        stager = data_flow_ops.MapStagingArea([dtypes.float32, dtypes.float32])
        stage = stager.put(pi, [x, v], [0, 1])
        k, (z, y) = stager.get(gi)
        y = math_ops.reduce_max(z * math_ops.matmul(y, y))
      sess.run(stage, feed_dict={x: -1, pi: 0})
      for i in range(10):
        _, yval = sess.run([stage, y], feed_dict={x: i, pi: i+1, gi:i})
        self.assertAllClose(
            4 * (i - 1) * (i - 1) * (i - 1) * 128, yval, rtol=1e-4)

  def testDictionary(self):
    with self.test_session(use_gpu=True) as sess:
      with ops.device('/cpu:0'):
        x = array_ops.placeholder(dtypes.float32)
        pi = array_ops.placeholder(dtypes.int64)
        gi = array_ops.placeholder(dtypes.int64)
        v = 2. * (array_ops.zeros([128, 128]) + x)
      with ops.device(test.gpu_device_name()):
        stager = data_flow_ops.MapStagingArea(
            [dtypes.float32, dtypes.float32],
            shapes=[[], [128, 128]],
            names=['x', 'v'])
        stage = stager.put(pi,{'x': x, 'v': v})
        key, ret = stager.get(gi)
        z = ret['x']
        y = ret['v']
        y = math_ops.reduce_max(z * math_ops.matmul(y, y))
      sess.run(stage, feed_dict={x: -1, pi: 0})
      for i in range(10):
        _, yval = sess.run([stage, y], feed_dict={x: i, pi: i+1, gi:i})
        self.assertAllClose(
            4 * (i - 1) * (i - 1) * (i - 1) * 128, yval, rtol=1e-4)

  def testColocation(self):
    gpu_dev = test.gpu_device_name()

    with ops.device('/cpu:0'):
      x = array_ops.placeholder(dtypes.float32)
      v = 2. * (array_ops.zeros([128, 128]) + x)
    with ops.device(gpu_dev):
      stager = data_flow_ops.MapStagingArea([dtypes.float32])
      y = stager.put(1, [v], [0])
      self.assertEqual(y.device, '/device:GPU:0' if gpu_dev
                                                 else gpu_dev)
    with ops.device('/cpu:0'):
      _, x = stager.get(1)
      y = stager.peek(1)
      _, z = stager.get()
      self.assertEqual(x.device, '/device:CPU:0')
      self.assertEqual(y.device, '/device:CPU:0')
      self.assertEqual(z.device, '/device:CPU:0')

  def testPeek(self):
    with ops.device('/cpu:0'):
      x = array_ops.placeholder(dtypes.int32, name='x')
      pi = array_ops.placeholder(dtypes.int64)
      gi = array_ops.placeholder(dtypes.int64)
      p = array_ops.placeholder(dtypes.int32, name='p')
    with ops.device(test.gpu_device_name()):
      stager = data_flow_ops.MapStagingArea([dtypes.int32, ], shapes=[[]])
      stage = stager.put(pi,[x], [0])
      peek = stager.peek(gi)
      size = stager.size()

    n = 10

    with self.test_session(use_gpu=True) as sess:
      for i in range(n):
        sess.run(stage, feed_dict={x:i, pi:i})

      for i in range(n):
        self.assertTrue(sess.run(peek, feed_dict={gi: i}) == i)

      self.assertTrue(sess.run(size) == 10)

  def testSizeAndClear(self):
    with ops.device('/cpu:0'):
      x = array_ops.placeholder(dtypes.float32, name='x')
      pi = array_ops.placeholder(dtypes.int64)
      gi = array_ops.placeholder(dtypes.int64)
      v = 2. * (array_ops.zeros([128, 128]) + x)
    with ops.device(test.gpu_device_name()):
      stager = data_flow_ops.MapStagingArea(
          [dtypes.float32, dtypes.float32],
          shapes=[[], [128, 128]],
          names=['x', 'v'])
      stage = stager.put(pi,{'x': x, 'v': v})
      size = stager.size()
      clear = stager.clear()

    with self.test_session(use_gpu=True) as sess:
      sess.run(stage, feed_dict={x: -1, pi: 3})
      self.assertEqual(sess.run(size), 1)
      sess.run(stage, feed_dict={x: -1, pi: 1})
      self.assertEqual(sess.run(size), 2)
      sess.run(clear)
      self.assertEqual(sess.run(size), 0)


  def testCapacity(self):
    capacity = 3

    with ops.device('/cpu:0'):
      x = array_ops.placeholder(dtypes.int32, name='x')
      pi = array_ops.placeholder(dtypes.int64, name='pi')
      gi = array_ops.placeholder(dtypes.int64, name='gi')
    with ops.device(test.gpu_device_name()):
      stager = data_flow_ops.MapStagingArea([dtypes.int32, ],
        capacity=capacity, shapes=[[]])

      stage = stager.put(pi, [x], [0])
      get = stager.get()
      size = stager.size()

    from six.moves import queue as Queue
    import threading

    queue = Queue.Queue()
    n = 5
    missed = 0

    with self.test_session(use_gpu=True) as sess:
      # Stage data in a separate thread which will block
      # when it hits the staging area's capacity and thus
      # not fill the queue with n tokens
      def thread_run():
        for i in range(n):
          sess.run(stage, feed_dict={x: i, pi: i})
          queue.put(0)

      t = threading.Thread(target=thread_run)
      t.start()

      # Get tokens from the queue, making notes of when we timeout
      for i in range(n):
        try:
          queue.get(timeout=0.05)
        except Queue.Empty:
          missed += 1

      # We timed out n - capacity times waiting for queue puts
      self.assertTrue(missed == n - capacity)

      # Clear the staging area out a bit
      for i in range(n - capacity):
        sess.run(get)

      # This should now succeed
      t.join()

      self.assertTrue(sess.run(size) == capacity)

      # Clear out the staging area completely
      for i in range(capacity):
        sess.run(get)

  def testMemoryLimit(self):
    memory_limit = 512*1024  # 512K
    chunk = 200*1024 # 256K
    capacity = memory_limit // chunk

    with ops.device('/cpu:0'):
      x = array_ops.placeholder(dtypes.uint8, name='x')
      pi = array_ops.placeholder(dtypes.int64, name='pi')
      gi = array_ops.placeholder(dtypes.int64, name='gi')
    with ops.device(test.gpu_device_name()):
      stager = data_flow_ops.MapStagingArea([dtypes.uint8],
        memory_limit=memory_limit, shapes=[[]])
      stage = stager.put(pi, [x], [0])
      get = stager.get()
      size = stager.size()

    from six.moves import queue as Queue
    import threading
    import numpy as np

    queue = Queue.Queue()
    n = 5
    missed = 0

    with self.test_session(use_gpu=True) as sess:
      # Stage data in a separate thread which will block
      # when it hits the staging area's capacity and thus
      # not fill the queue with n tokens
      def thread_run():
        for i in range(n):
          sess.run(stage, feed_dict={x: np.full(chunk, i, dtype=np.uint8),
                                    pi: i})
          queue.put(0)

      t = threading.Thread(target=thread_run)
      t.start()

      # Get tokens from the queue, making notes of when we timeout
      for i in range(n):
        try:
          queue.get(timeout=0.05)
        except Queue.Empty:
          missed += 1

      # We timed out n - capacity times waiting for queue puts
      self.assertTrue(missed == n - capacity)

      # Clear the staging area out a bit
      for i in range(n - capacity):
        sess.run(get)

      # This should now succeed
      t.join()

      self.assertTrue(sess.run(size) == capacity)

      # Clear out the staging area completely
      for i in range(capacity):
        sess.run(get)

  def testOrdering(self):
    import six
    import random

    with ops.device('/cpu:0'):
      x = array_ops.placeholder(dtypes.int32, name='x')
      pi = array_ops.placeholder(dtypes.int64, name='pi')
      gi = array_ops.placeholder(dtypes.int64, name='gi')
    with ops.device(test.gpu_device_name()):
      stager = data_flow_ops.MapStagingArea([dtypes.int32, ],
        shapes=[[]], ordered=True)
      stage = stager.put(pi, [x], [0])
      get = stager.get()
      size = stager.size()

    n = 10

    with self.test_session(use_gpu=True) as sess:
      # Keys n-1..0
      keys = list(reversed(six.moves.range(n)))

      for i in keys:
        sess.run(stage, feed_dict={pi: i, x: i})

      self.assertTrue(sess.run(size) == n)

      # Check that key, values come out in ascending order
      for i, k in enumerate(reversed(keys)):
        get_key, values = sess.run(get)
        self.assertTrue(i == k == get_key == values)

      self.assertTrue(sess.run(size) == 0)

  def testBarrier(self):
    with self.test_session(use_gpu=True) as sess:
      with ops.device('/cpu:0'):
        x = array_ops.placeholder(dtypes.float32)
        f = array_ops.placeholder(dtypes.float32)
        v = array_ops.placeholder(dtypes.float32)
        pi = array_ops.placeholder(dtypes.int64)
        gi = array_ops.placeholder(dtypes.int64)
      with ops.device(test.gpu_device_name()):
        # Test barrier with dictionary
        stager = data_flow_ops.MapStagingArea(
            [dtypes.float32, dtypes.float32, dtypes.float32],
            names=['x', 'v', 'f'])
        stage_xf = stager.put(pi,{'x': x, 'f': f})
        stage_v = stager.put(pi, {'v': v})
        key, ret = stager.get(gi)
        size = stager.size()
        isize = stager.incomplete_size()

        # 0 complete and incomplete entries
        self.assertTrue(sess.run([size, isize]) == [0, 0])
        # Stage key 0, x and f tuple entries
        sess.run(stage_xf, feed_dict={pi: 0, x: 1, f: 2})
        self.assertTrue(sess.run([size, isize]) == [0, 1])
        # Stage key 1, x and f tuple entries
        sess.run(stage_xf, feed_dict={pi: 1, x: 1, f: 2})
        self.assertTrue(sess.run([size, isize]) == [0, 2])

        # Now complete key 0 with tuple entry v
        sess.run(stage_v, feed_dict={pi: 0, v: 1})
        # 1 complete and 1 incomplete entry
        self.assertTrue(sess.run([size, isize]) == [1, 1])
        # We can now obtain tuple associated with key 0
        self.assertTrue(sess.run([key, ret], feed_dict={gi:0})
                                == [0, { 'x':1, 'f':2, 'v':1}])

        # 0 complete and 1 incomplete entry
        self.assertTrue(sess.run([size, isize]) == [0, 1])
        # Now complete key 1 with tuple entry v
        sess.run(stage_v, feed_dict={pi: 1, v: 3})
        # We can now obtain tuple associated with key 1
        self.assertTrue(sess.run([key, ret], feed_dict={gi:1})
                                == [1, { 'x':1, 'f':2, 'v':3}])

        # Test again with index inserts
        stager = data_flow_ops.MapStagingArea(
            [dtypes.float32, dtypes.float32, dtypes.float32])
        stage_xf = stager.put(pi, [x, f], [0, 2])
        stage_v = stager.put(pi, [v], [1])
        key, ret = stager.get(gi)
        size = stager.size()
        isize = stager.incomplete_size()

        # 0 complete and incomplete entries
        self.assertTrue(sess.run([size, isize]) == [0, 0])
        # Stage key 0, x and f tuple entries
        sess.run(stage_xf, feed_dict={pi: 0, x: 1, f: 2})
        self.assertTrue(sess.run([size, isize]) == [0, 1])
        # Stage key 1, x and f tuple entries
        sess.run(stage_xf, feed_dict={pi: 1, x: 1, f: 2})
        self.assertTrue(sess.run([size, isize]) == [0, 2])

        # Now complete key 0 with tuple entry v
        sess.run(stage_v, feed_dict={pi: 0, v: 1})
        # 1 complete and 1 incomplete entry
        self.assertTrue(sess.run([size, isize]) == [1, 1])
        # We can now obtain tuple associated with key 0
        self.assertTrue(sess.run([key, ret], feed_dict={gi:0})
                                == [0, [1, 1, 2]])

        # 0 complete and 1 incomplete entry
        self.assertTrue(sess.run([size, isize]) == [0, 1])
        # Now complete key 1 with tuple entry v
        sess.run(stage_v, feed_dict={pi: 1, v: 3})
        # We can now obtain tuple associated with key 1
        self.assertTrue(sess.run([key, ret], feed_dict={gi:1})
                                == [1, [1,3, 2]])


if __name__ == '__main__':
  test.main()
