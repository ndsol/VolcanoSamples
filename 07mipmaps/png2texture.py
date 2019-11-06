#!/usr/bin/env python3
# Copyright (c) 2017-2018 the Volcano Authors. Licensed under GPLv3.
import errno
import os
import platform
from select import select
import subprocess
import sys

class runcmd(object):
  def __init__(self, env):
    self.have_out = False
    self.have_err = False
    self.env = env
    self.p = None

  def handle_output(self, is_stdout, out, data):
    out.write(data.decode(sys.stdout.encoding))
    out.flush()

  def run(self, args):
    path_saved = os.environ["PATH"]
    if "PATH" in self.env:
      os.environ["PATH"] = self.env["PATH"]
    try:
      import pty
      masters, slaves = zip(pty.openpty(), pty.openpty())
      self.p = subprocess.Popen(args, stdout=slaves[0], stderr=slaves[1],
                                env = self.env)
      for fd in slaves: os.close(fd)
      readable = { masters[0]: sys.stdout, masters[1]: sys.stderr }
      while readable:
        for fd in select(readable, [], [])[0]:
          try: data = os.read(fd, 1024)
          except OSError as e:
            if e.errno != errno.EIO: raise
            del readable[fd]
            continue
          if not data:
            del readable[fd]
            continue
          self.handle_output(fd == masters[0], readable[fd], data)
          if fd != masters[0]:
            self.have_err = True
          else:
            self.have_out = True
      self.p.wait()
    except ImportError:
      if sys.platform != "win32":
        raise
      self.p = subprocess.Popen(args, shell=True)
      (o, e) = self.p.communicate()
      self.have_out = False
      self.have_err = False
      if o:
        self.have_out = True
        self.handle_output(True, sys.stdout, o)
      if e:
        self.have_err = True
        self.handle_output(False, sys.stdout, e)
    os.environ["PATH"] = path_saved


class runcmd_ignore_output(runcmd):
  def run(self, args):
    super(runcmd_ignore_output, self).run(args)
    if self.p.returncode != 0:
      print("run(%s) returned %d" % (args, self.p.returncode))
      sys.exit(1)

    #if self.have_err:
    #  print("run(%s) spit something out on stderr" % args)
    #  sys.exit(1)

if __name__ == "__main__":
  if len(sys.argv) != 3 and len(sys.argv) != 4:
    print("Usage: %s input output" % sys.argv[0])
    sys.exit(1)

  try: os.makedirs(os.path.dirname(sys.argv[2]))
  except OSError as e:
    if e.errno != errno.EEXIST: raise

  cenv = os.environ.copy()
  cenv["VK_INSTANCE_LAYERS"] = "VK_LAYER_LUNARG_standard_validation"
  prog = "0701png2texture"
  if platform.system() != "Windows":
    prog = "./" + prog
  runcmd_ignore_output(cenv).run([ prog ] + sys.argv[1:])
