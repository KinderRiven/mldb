#!/usr/bin/env python
# Copyright Datacratic 2016
# Author: Jean Raby <jean@datacratic.com>

# Wrapper around tee -a
# Read from stdin and write to logfile and to stdout

import functools
import os
import fcntl
import sys
import time

import tornado.web

from tornado.ioloop import IOLoop
from datetime import datetime
from collections import namedtuple, deque

RINGBUFSIZE = 1024

LogLine = namedtuple('LogLine', ['dt', 'data', ])

logline_cnt = 0

def stdin_ready(f, ringbuf, fd, events):
  global logline_cnt
  if events & IOLoop.READ:
    try:
      for line in f:
        logline = LogLine(dt=datetime.utcnow(), data=line)
        ringbuf.append(logline)
        logline_cnt += 1
        sys.stdout.write(line)
    except IOError:
      # If we get a EWOULDBLOCK, continue
      # EOF handled below
      pass
  if events & IOLoop.ERROR:
      exit(0)

class LogsMldbHandler(tornado.web.RequestHandler):
  def get(self):
    self.write('<html><body><pre>\n')
    for l in log_lines_ringbuf:
      self.write("%s %s" % (l.dt.isoformat(), l.data))
    self.write('</pre><a name=end></body></html>\n')


if __name__ == "__main__":

  log_lines_ringbuf = deque(maxlen=RINGBUFSIZE)
  io_loop = IOLoop.current()

  # set stdin to non blocking mode for use with tornado
  fl = fcntl.fcntl(sys.stdin.fileno(), fcntl.F_GETFL)
  fcntl.fcntl(sys.stdin.fileno(), fcntl.F_SETFL, fl | os.O_NONBLOCK)

  callback = functools.partial(stdin_ready, sys.stdin, log_lines_ringbuf)
  io_loop.add_handler(sys.stdin.fileno(), callback,
                      io_loop.READ | io_loop.ERROR)

  app = tornado.web.Application([ ("/logs/mldb", LogsMldbHandler) ])
  app.listen(12346, '0.0.0.0')

  try:
    t1 = time.time()
    io_loop.start()
  except KeyboardInterrupt:
    total_time = time.time() - t1
    sys.stderr.write("Got %d lines in %d sec: %f lines/s\n" % (logline_cnt, total_time, logline_cnt/total_time))
    raise
