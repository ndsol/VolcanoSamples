#!/usr/bin/env python3
# Copyright (c) 2017-2018 the Volcano Authors. Licensed under GPLv3.

import argparse
import base64
import distutils.spawn
import errno
import hashlib
import os
import platform
import pty
import re
from select import select
import shutil
import ssl
import subprocess
import sys
import time
import urllib
import urllib.request
import zipfile

class runcmd(object):
  def __init__(self, env):
    self.have_out = False
    self.have_err = False
    self.env = env
    self.p = None

  def handle_output(self, is_stdout, out, data):
    out.write(data.decode('utf8', 'ignore')
        .encode(sys.stdout.encoding, 'replace').decode(sys.stdout.encoding))
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


class runcmd_return_stdout(runcmd):
  def __init__(self, env, max_out_len):
    super(runcmd_return_stdout, self).__init__(env)
    self.max_out_len = max_out_len
    self.result = b""
    self.also_stderr = False

  def and_stderr(self):
    self.also_stderr = True
    return self

  def handle_output(self, is_stdout, out, data):
    if is_stdout or self.also_stderr:
      self.result += data
      if len(self.result) > self.max_out_len:
        self.p.kill()

  def run(self, args):
    super(runcmd_return_stdout, self).run(args)
    if len(self.result) > self.max_out_len:
      print("run(%s) got more than %d bytes" % (args, self.max_out_len))
      sys.exit(1)
    r = (self.result.decode('utf8', 'ignore')
                .encode(sys.stdout.encoding, 'replace')
                .decode(sys.stdout.encoding))
    if self.p.returncode != 0:
      print("run(%s) returned %d" % (args, self.p.returncode))
      print(r)
      sys.exit(1)
    return r


def git_rev_parse(args):
  return runcmd_return_stdout(os.environ.copy(), 4096).run([
    "git", "rev-parse" ] + args).strip()

# "git rev-parse --show-toplevel" shows the top level dir of a git repo.
# If this command is not run in a git repo, fail now.
git_toplevel = git_rev_parse(["--show-toplevel"])

def writeprogress(from0to1, msg = ""):
  if isinstance(from0to1, int):
      from0to1 = float(from0to1)
  if not isinstance(from0to1, float):
      from0to1 = 0
      msg += " error: writeprogress requires a float argument from 0..1\n"
  sys.stdout.write("\r\x1b[K%4.2f%% %s" % (from0to1*100, msg))
  sys.stdout.flush()


def clearprogress():
  sys.stdout.write("\r\x1b[K")


def urllib_http_get(url, headers = None):
  ctx = ssl.create_default_context()
  ctx.check_hostname = False
  ctx.verify_mode = ssl.CERT_NONE

  opener = urllib.request.build_opener(
      urllib.request.HTTPSHandler(context = ctx))
  if headers is not None:
    opener.addheaders = headers
  return opener.open(url)


# android_environ sets up the build environment.
class android_environ(object):
  # max_android_sdk is the NN in "platforms;android-NN" returned by sdkmanager.
  # This version may NOT be installed but it can be requested.
  max_android_sdk = None
  # cur_android_sdk is the NN of "$ANDROID_HOME/platforms/android-NN".
  # This version MUST be installed.
  cur_android_sdk = None
  # max_buildtools is the X.Y.Z in "build-tools;X.Y.Z" returned by sdkmanager.
  # This version may NOT be installed but it can be requested.
  max_buildtools = None
  # cur_buildtools is the X.Y.Z dir "$ANDROID_HOME/build-tools/X.Y.Z".
  # This version MUST be installed.
  cur_buildtools = None

  def __init__(self):
    self.installto = os.path.join(git_toplevel, "..")
    self.java_ver = []
    self.jaxb = [
      {
        "url":"https://repo1.maven.org/maven2/javax/activation/activation/1.1.1/activation-1.1.1.jar",
        "len":69409,
      },
      {
        "url":"https://repo1.maven.org/maven2/javax/xml/jaxb-impl/2.1/jaxb-impl-2.1.jar",
        "len":827108,
      },
      {
        "url":"https://repo1.maven.org/maven2/org/glassfish/jaxb/jaxb-xjc/2.3.2/jaxb-xjc-2.3.2.jar",
        "len":903197,
      },
      {
        "url":"https://repo1.maven.org/maven2/org/glassfish/jaxb/jaxb-core/2.3.0.1/jaxb-core-2.3.0.1.jar",
        "len":157148,
      },
      {
        "url":"https://repo1.maven.org/maven2/org/glassfish/jaxb/jaxb-jxc/2.3.2/jaxb-jxc-2.3.2.jar",
        "len":120605,
      },
      {
        "url":"https://repo1.maven.org/maven2/javax/xml/bind/jaxb-api/2.3.1/jaxb-api-2.3.1.jar",
        "len":128076,
      },
    ]

  # get_sdk_tools populates fileset with platform-specific information
  def get_sdk_tools(self, fileset):
    dl_android = "https://dl.google.com/android/repository/"
    ndk = {}
    if sys.platform == "darwin":
      os = "darwin"
      sdk_tools = {
        "url":dl_android + "sdk-tools-darwin-4333796.zip",
        "len":103022432,
        "sha256":"ecb29358bc0f13d7c2fa0f9290135a5b" +
                 "608e38434aad9bf7067d0252c160853e",
        "sha512":"85edfded45e818cd5673346bc2bc5c3ee4b3b4dc5a507f7c9d7b9153c898e"+
          "5abb5be6975ae1721d787cf5a7fe3e2a28294ec49e7b6e1c42c4132ae3b5bdb762d"
      }
      ndk["14"] = {
        "len":824705073,
        "sha256":"f5373dcb8ddc1ba8a4ccee864cba2cbd" +
                 "f500b2a32d6497378dfd32b375a8e6fa",
        "sha512":"2590a77e0c07211d0d61291ec00c84e979734af46472b55a2ab74e70993f"+
          "e422b6c4adaed3d3619a3f56fcc408a876f2141c7a3afcf7a5832df4161a810ba84b"
      }
      ndk["15"] = {
        "len":960251267,
        "sha256":"846ce931e27668665fef9d31faa2cce6" +
                 "83ccebe4c6ca0e68f0eb9920bc70e55f",
        "sha512":"27ff587ef40d8a1dae613c632585557b99fda4f6b54862a6ddd4fcae9fd9"+
          "e120004ff3ba46f6d8b39353de91dfd242c41fa1b39d78b283c85a1fac99ad624d11"
      }
      ndk["16"] = {
        "len":839630771,
        "sha256":"9654a692ed97713e35154bfcacb0028f" +
                 "dc368128d636326f9644ed83eec5d88b",
        "sha512":"f2ef452cb9db8639354da6bcc31fa0d393e6fd13ef43d88f62330de8b8cc"+
          "b3df1aca66d2be421b01b34bcacd52d21cafecb7aecc607dc10cd3176f1e1bb5d3ef"
      }
      ndk["17"] = {
        "len":675091485,
        "sha256":"e3d866c613583763848e8f97dac7a9e3" +
                 "74956bd2764e522c64ee0470f1eb3e86",
        "sha512":"9e25c06c217d4f52cd05e21fc288380820410f3f75f5106e2881e4a8ac97"+
          "4f404c1bc3c0a8c961ac6fb843d336aed3899f9c7130585df18c813ce93a6978acf2"
      }
      ndk["18"] = {
        "len":542911996,
        "sha256":"dd6524c3cc91725b5c39370f6deb27d8" +
                 "eee056e2bb2efef0a8008b4ca6d83891",
        "sha512":"b3df0437967fb1b37f06000398ded71b09c4fd0bb8d0a481ec165c210652"+
          "b8926141dc96cb474edad5aad4d2414cec266a8d9dc7730f13b074d41837db9b1cd2"
      }
      ndk["19"] = {
        "len":807630656,
        "sha256":"4eeaddcb4bb58b2a10a9712f9b8e84ea" +
                 "83889786a88923879e973058f59281cf",
        "sha512":"1bcc081c89d1b48b52ea2c644f59277c0736ce164c2054e5cf63bd314fd2"+
          "6fad0b1b90531289bdc2b67179d094c7ba1a4b1791660ad571da8ae3901343b066b2"
      }
    elif sys.platform[:3] == "win":
      os = "windows"
      sdk_tools = {
        "url":dl_android + "sdk-tools-windows-4333796.zip",
        "len":156136858,
        "sha256":"7e81d69c303e47a4f0e748a6352d85cd" +
                 "0c8fd90a5a95ae4e076b5e5f960d3c7a",
        "sha512":"5e4d525452d3e5a93d958dd34334ef38b428de730b7aa4c5ec1429e15fd4"+
          "72fe52d9a8066d8d3376ae57cba12f188c0591d084da9875378276a67bf246e228da"
      }
      # same sdk for win32 or win64 but different ndk
      if not platform.machine().endswith("64"):
        ndk["14"] = {
          "len":707533928,
          "sha256":"5c9ae0268f638ef63783e12265ae8369" +
                   "d1c47badf5c3806990a7cd08ae3d7edf",
          "sha512":"aab959446126aa32712e9be9d67f789a1f460ddf7b72c27adbd4f11ef1"+
                   "20a71eda1e9fc20311807bab1f65d1dfa1df07135c390da618e14b1233"+
                   "7ec1fc11235c"
        }
        ndk["15"] = {
          "len":784778144,
          "sha256":"27b233edf9a5a7114fbfc858d7caf6ef" +
                   "a3abb16e563d7ea527639758fb03c313",
          "sha512":"b64b3645b84c306811a7fbb3955f041a4dc60d47cde450686d6cc2e30d"+
                   "2eec95700ef1d8b2b07ac4f0b6d2b57dda359eb9832f8282d052337ae1"+
                   "de1306d97021"
        }
        ndk["16"] = {
          "len":656720029,
          "sha256":"a67c1152eda390de715e1cdb53b1e595" +
                   "9bcebf233a02326dc0193795c6eda8d7",
          "sha512":"323c014a41e4ad87db8248928970e456a7e083572e321d55699162369f"+
                   "594e734a83f31d0835b53e858fcb5c4b0f9bd9448c4829724253a9dffa"+
                   "2a506ee355d2"
        }
        ndk["17"] = {
          "len":608358310,
          "sha256":"e9f4ea9aa60dc863c2edd821b893dcbc" +
                   "849e97858b1eda3d3171a31c73f2a83a",
          "sha512":"acc3a303e9e8e3549aae59a935e6fb3e9da388f8022353f913ed5b4d6c8"+
                   "290af4a6be31adb06e93665d1cfd9e09a3b89a65e5aca2d327fc5b723dd"+
                   "3c1d32cefc"
        }
        ndk["18"] = {
          "len":504605336,
          "sha256":"205776bf1b1b8e6b624e301063b57d49" +
                   "fa93e3c6da0404fdf38d5795f29c4f2d",
          "sha512":"f1fc7ef27edebe87b66211fd79e785097220b75f7e4673092fa54c34f89"+
                   "49af71f65693404ae5dee5abc32f2202669bc8150e4b24131d66818268b"+
                   "561c87ec79"
        }
        ndk["19"] = {
          "len":778598286,
          "sha256":"800c3c6ba616ddf25097d43566d5d574" +
                   "f9e6c0a10538bf60dd5be0e024f732cd",
          "sha512":"c397b848a9e63830083b4370d4b53040efcb1d5dc3e55747f6ef38fa54b"+
                   "4c3fdcb4c78f1f0ba87ca1e7def725b6687f017180c7ef60ae71779ac3a"+
                   "d79db7166c"
        }
      else:
        ndk["14"] = {
          "len":769151176,
          "sha256":"6c90f9462066ddc15035ac5f5fc51bcf" +
                   "ac577dc31b31538a4e24f812c869b9c9",
          "sha512":"0ace556c7ad0dd6a746207b3b60e9b9545d8c58c338ec632fa847cea6b"+
                   "d74e63b70bfa103477b3574290718859987ab3ed65d906efd463f1273d"+
                   "7983b4a80481"
        }
        ndk["15"] = {
          "len":849733996,
          "sha256":"6f3785cbe7bd90dd0383e88c20aaa656" +
                   "b5a5e38a6cdc389e667e3b2c636bcd8d",
          "sha512":"637ae4b42e04867948fb5b4367790ab505de9242373071d8b91f7e4afd"+
                   "4fbe70ccb1e030f28093ffc594dc6a1300b88c3c433f3559d4459087e0"+
                   "a4873a6c6089"
        }
        ndk["16"] = {
          "len":723301086,
          "sha256":"4c6b39939b29dfd05e27c97caf588f26" +
                   "b611f89fe95aad1c987278bd1267b562",
          "sha512":"00c5236f97d71c5fe1736be99c4958571177f7f6d09def60e74ea7092d"+
                   "838f722bd563b1a96c117a29584957caae2f7280ac5a3f3dd2e1ab6523"+
                   "e7893655d72e"
        }
        ndk["17"] = {
          "len":650626501,
          "sha256":"8a2632b6c52d8b327a66240c276f0922" +
                   "6089ef4516c43476f08723a668d30c30",
          "sha512":"bbf6b18ceb40aa0c956d3891685cbd2ebf38034c4d3b1402aa4274fe844"+
                   "2a63f94535d08aed770d092286a3384e024bf2940aadfbc7eebac84de7f"+
                   "a4bbf08251"
        }
        ndk["18"] = {
          "len":522489470,
          "sha256":"601b10d8c48486338530d55785ecb87f" +
                   "24ee4a98adad2ee8352f72552d434362",
          "sha512":"593ae8b9d1c5cc3d21ec12495d19dc8f14b7a24f4e61b594b768cc779e4"+
                   "7983a0db1c74783eeeab0f84684eadc85573f2c0e822f80f49611160fdd"+
                   "31562a8f30"
        }
        ndk["19"] = {
          "len":796051997,
          "sha256":"0faf708c9837a921cae5262745f58571" +
                   "62614bb9689a0d188780d12ea93a2c18",
          "sha512":"f1c4165bd9470df46675f95e438b4febe7da9e483d8f3a570c70277f47c"+
                   "1a0ab5009820a748b64ea02f101eab085f7a865dc4eb399a85761ff3365"+
                   "492da4f40a"
        }
    elif sys.platform[:5] == "linux": # linux or linux2 have been seen
      os = "linux"
      sdk_tools = {
        "url":dl_android + "sdk-tools-linux-4333796.zip",
        "len":154582459,
        "sha256":"92ffee5a1d98d856634e8b71132e8a95" +
                 "d96c83a63fde1099be3d86df3106def9",
        "sha512":"5050bf5cd8327c59f420f2072369d299e9193a3d697740b5b253199635299"+
          "f90f34f4c4da2467f3af356c5c6226adfce9aa311ef2c9673a6ba664ab5c3173d18"
      }
      ndk["14"] = {
        "len":840626594,
        "sha256":"0ecc2017802924cf81fffc0f51d342e3" +
                 "e69de6343da892ac9fa1cd79bc106024",
        "sha512":"24435267fc5acae559aa5159f7c895ce5ea0cbb8ef966bb8ff0dadffcadc"+
          "cbe46bc3880d285bf4e411ef78632cf2f862408e7b2b41ebca51078b41eac66a301a"
      }
      ndk["15"] = {
        "len":974976754,
        "sha256":"f01788946733bf6294a36727b99366a1" +
                 "8369904eb068a599dde8cca2c1d2ba3c",
        "sha512":"abe7f111ca51d9bb07a9fb6f9f6cc8f0195be056c8d8d996cddf4e7df101"+
          "d120dab1afce71a1670e4f29f7b6462eb9863fbf7c86dcd9a21b7cb4bcea822cbe4f"
      }
      ndk["16"] = {
        "len":852525873,
        "sha256":"bcdea4f5353773b2ffa85b5a9a2ae355" +
                 "44ce88ec5b507301d8cf6a76b765d901",
        "sha512":"94cd879925ee3174a9267e7da2d18d71874173976b362101ec06598a94b6"+
          "587a33671e54bbbce5778c04418aacbb831e98386c16f6cde04574ea8c8589553dd7"
      }
      ndk["17"] = {
        "len":709387703,
        "sha256":"3f541adbd0330a9205ba12697f6d04ec" +
                 "90752c53d6b622101a2a8a856e816589",
        "sha512":"b8ad2aae0ea8caa7043c390c94c638c9499db22050c46b5e4f4b005fef6b"+
          "47b7abdfb76c340c5a7e6024d8e6be926e07ab5556002c02919ba88f8b9469ed831e"
      }
      ndk["18"] = {
        "len":557038702,
        "sha256":"4f61cbe4bbf6406aa5ef2ae871def780" +
                 "10eed6271af72de83f8bd0b07a9fd3fd",
        "sha512":"a35ab95ece52819194a3874fd210abe5c25905212c4aafe5d75c465c1473"+
          "9a46340d1ff0944ad93ffbbc9c0d86107119399d4f60ec6c5f080758008e75c19617"
      }
      ndk["19"] = {
        "len":823376982,
        "sha256":"4c62514ec9c2309315fd84da6d524656" +
                 "51cdb68605058f231f1e480fcf2692e1",
        "sha512":"9042970d78baf48a3286edb765103846ce0ba086888de94212a78129402d"+
          "e5ca3db7c7d5eba81e9c89af1cb98ad99ea20a0f593bfb1702b99e101555a433d670"
      }
    fileset.append(sdk_tools)
    # NDK r14-17 have not been tested in a long time, and are likely to fail!
    r = "19"
    ndkrc = { # Map the number to the number-plus-little-letter code.
      "14":"r14b",
      "15":"r15c",
      "16":"r16b",
      "17":"r17c",
      "18":"r18b",
      "19":"r19c",
    }
    if r not in ndkrc:
      print("ERROR: NDK r%s is not defined in build-android.py" % r)
      sys.exit(1)
    ndkfiles = ndk[r]
    ndkfiles["url"] = "%sandroid-ndk-%s-%s-%s.zip" % (
      dl_android, ndkrc[r], os,
      "x86_64" if platform.machine().endswith("64") else "x86")
    fileset.append(ndkfiles)

  # download_file only downloads the one in fileset[i], but the progress bar
  # denominator is correct, showing the total of all the files in the fileset.
  def download_file(self, fileset, i):
    totallen = 0
    prevcount = 0
    for x in range(len(fileset)):
      if x < i: prevcount += fileset[x]["len"]
      totallen += fileset[x]["len"]

    md256 = hashlib.sha256()
    md512 = hashlib.sha512()
    bytecount = 0
    local_filename = fileset[i]["local"]
    if os.path.isfile(local_filename):
      with open(local_filename, "rb") as f:
        for chunk in iter(lambda: f.read(131072), b""):
          bytecount += len(chunk)
          md256.update(chunk)
          md512.update(chunk)
      if bytecount == fileset[i]["len"]:
        if (md256.hexdigest() != fileset[i]["sha256"] or
            md512.hexdigest() != fileset[i]["sha512"]):
          print("WARNING: %s sha256=%s sha512=%s" % (
              local_filename, md256.hexdigest(), md512.hexdigest()))
          print("want: sha256=%s sha512=%s" % (
              fileset[i]["sha256"], fileset[i]["sha512"]))
        print("DONE %s" % fileset[i]["url"])
        return
      elif bytecount > fileset[i]["len"]:
        print("INVALID DOWNLOAD: %s len=%d want %d" % (
            fileset[i]["url"], bytecount, fileset[i]["len"]))
        sys.exit(1)
      # resume download
      print("resume %s" % fileset[i]["url"])
      headers = [("Range", "bytes=%d-" % bytecount)]
    else:
      # begin fresh download
      print(fileset[i]["url"])
      headers = None

    r = urllib_http_get(fileset[i]["url"], headers = headers)
    with open(local_filename, "ab") as f:
      while True:
        chunk = r.read(131072)
        if not chunk:
          break
        bytecount += len(chunk)
        writeprogress(float(prevcount + bytecount) / totallen, local_filename)
        md256.update(chunk)
        md512.update(chunk)
        f.write(chunk)
    r.close()
    clearprogress()
    if (md256.hexdigest() != fileset[i]["sha256"] or
        md512.hexdigest() != fileset[i]["sha512"] or
        bytecount != fileset[i]["len"]):
      print("WARNING: download %s has size=%d sha256=%s sha512=%s" % (
        local_filename, bytecount, md256.hexdigest(), md512.hexdigest()))
      print("WARNING: wanted: size=%d sha256=%s sha512=%s" % (
        fileset[i]["len"], fileset[i]["sha256"], fileset[i]["sha512"]))
      print("")
      sys.stdout.write("Someone may be trying to attack you. Continue? (Y/n): ")
      sys.stdout.flush()
      python3_input = None
      try:
        # attempt the python2 form
        python3_input = raw_input
      except NameError:
        # attempt the python3 form
        python3_input = input
      line = python3_input()
      if line != "" and line != "y" and line != "Y":
        print(" you typed \"%s\". exiting." % line)
        sys.exit(1)

  # find_java() locates the Java JVM
  def find_java(self):
    self.java = distutils.spawn.find_executable("java")
    if self.java is None and "JAVA_HOME" in os.environ:
      self.java = distutils.spawn.find_executable(
          os.path.join(os.environ["JAVA_HOME"], "bin", "java"))
    if self.java is None:
      print("ERROR: JAVA_HOME is not set and no 'java' command in your PATH.")
      print("")
      print("Please set JAVA_HOME in your environment to match the location of")
      print("your Java installation.")
      sys.exit(1)

    # Check java version
    jresult = runcmd_return_stdout(os.environ.copy(), 65536).and_stderr(
      ).run([self.java, "-version" ])
    jdelim = " version \""
    if jresult.find(jdelim) < 0:
      print("failed to get version from \"%s -version\":" % java)
      print(jresult)
      sys.exit(1)
    for line in jresult.split("\n"):
      p = line.find(jdelim)
      v = line[p + len(jdelim):]
      p = v.find("\"")
      if p < 0:
        print("failed to parse version from \"%s -version\":" % java)
        print(java_ver)
        sys.exit(1)
      java_ver=v[:p]
      break
    self.calc_java_ver(java_ver)

  # calc_java_ver finds all the int-valued parts of the java version string s
  def calc_java_ver(self, s):
    self.java_ver = []
    for code in s.split("."):
      for hyphens in code.split("_"):
        for val in hyphens.split("-"):
          try:
            i = int(val)
            self.java_ver.append(i)
          except ValueError as e:
            # stop parsing when a non int-valued part is found
            return

  # run_sdkm() runs the android sdkmanager tool
  def run_sdkm(self, sdkm, args, ignore_output = False):
    if not self.java_ver:
      print("run_sdkm(%s, %s): must call self.find_java() first" % (sdkm, args))
      sys.exit(1)
    cenv = os.environ.copy()
    if self.java_ver[0] > 10:
      # See https://stackoverflow.com/questions/53076422
      show_warning = True
      libdest = os.path.join(self.installto, "jaxb")
      try: os.makedirs(libdest)
      except OSError as e:
        if e.errno != errno.EEXIST: raise
      totallen = 0
      for lib in self.jaxb:
        totallen += lib["len"]
      progress = 0
      classpath = []
      did_not_show_urls = []
      for lib in self.jaxb:
        url = lib["url"]
        local_filename = os.path.join(libdest, url.split("/")[-1])
        classpath.append(local_filename)
        bytecount = 0
        with open(local_filename, "ab") as f:
          if f.tell() >= lib["len"]:
            did_not_show_urls.append(url)
            progress += f.tell()
            continue
          f.seek(0)
          f.truncate()
          if show_warning:
            print("WARNING: java %s removes Java EE." % self.java_ver)
            print("WARNING: downloading jaxb...")
            for url in did_not_show_urls:
              print(url)
          show_warning = False
          print(url)
          r = urllib_http_get(url)
          while True:
            chunk = r.read(131072)
            if not chunk:
              break
            bytecount += len(chunk)
            writeprogress(float(progress + bytecount) / totallen,
                          local_filename)
            f.write(chunk)
        r.close()
        progress += bytecount
        clearprogress()
      deps = [
          "dvlib",
          "jimfs",
          "jsr305",
          "repository",
          "j2objc-annotations",
          "layoutlib-api",
          "gson",
          "httpcore",
          "commons-logging",
          "commons-compress",
          "annotations",
          "error_prone_annotations",
          "animal-sniffer-annotations",
          "httpclient",
          "commons-codec",
          "common",
          "kxml2",
          "httpmime",
          "annotations",
          "sdklib",
          "guava",
        ]
      if sdkm[0] != os.sep:
        dpath = os.path.normpath(os.path.join(
            os.getcwd(), os.path.split(sdkm)[0], ".."))
      else:
        dpath = os.path.normpath(os.path.join(os.path.split(sdkm)[0], ".."))
      for f in os.listdir(os.path.join(dpath, "lib")):
        for dep in deps:
          if f.startswith(dep) and f.endswith(".jar"):
            classpath.append(os.path.join(dpath, "lib", f))
            break
      cmd = [
          self.java,
          "-Dcom.android.sdklib.toolsdir=%s" % dpath,
          "-classpath", ":".join(classpath),
          "com.android.sdklib.tool.sdkmanager.SdkManagerCli",
        ] + args
    elif self.java_ver[0] > 8:
      # See https://stackoverflow.com/questions/46402772
      cenv["JAVA_OPTS"] = "-XX:+IgnoreUnrecognizedVMOptions --add-modules java.se.ee"
      cmd = [sdkm] + args
    else:
      cmd = [sdkm] + args
    if ignore_output:
      runcmd_ignore_output(cenv).run(cmd)
      return None
    else:
      return runcmd_return_stdout(cenv, 65536).run(cmd)

  # get_sdkm_list_max returns the latest "build-tools" version.
  def get_sdkm_list_max(self, sdkm):
    sdkm_list = self.run_sdkm(sdkm, ["--list"])
    pkg_list_linecount = 0
    self.max_buildtools = ""
    self.max_android_sdk = 0
    for line in sdkm_list.splitlines():
      if line == "Available Packages:" or pkg_list_linecount > 0:
        pkg_list_linecount += 1
        if pkg_list_linecount > 3:
          fields = line.split("|")
          if len(fields) != 3:
            pkg_list_linecount = 0
            continue
          name = fields[0].strip()
          #ver = fields[1].strip()
          #description = fields[2].strip()
          if name[:12] == "build-tools;":
            self.max_buildtools = max(self.max_buildtools, name[12:])
          elif name[:18] == "platforms;android-":
            self.max_android_sdk = max(self.max_android_sdk, int(name[18:]))
    if self.max_buildtools == "":
      print("%s --list: no build-tools packages found" % sdkm)
      sys.exit(1)
    if self.max_android_sdk == 0:
      print("%s --list: no platforms;android found" % sdkm)
      sys.exit(1)

  # find_android_sdk locates or downloads + installs the android SDK and NDK.
  # The goals in priority order are:
  # 1. Successfully build an APK. Just do what it takes.
  # 2. Download + install whatever is needed to build the APK.
  #    if they have already been downloaded, this *should* find them. If it
  #    doesn't, that may indicate a bug.
  # 3. If there's no other hint where to put things, pick a reasonable place to
  #    put SDK files, one that doesn't require superuser permissions.
  def find_android_sdk(self, minsdk):
    self.find_java()

    jarsigner = distutils.spawn.find_executable("jarsigner")
    if jarsigner is None:
      print("ERROR: jarsigner not found. Please install jarsigner.")
      sys.exit(1)

    sdkm = distutils.spawn.find_executable("sdkmanager")

    if sdkm is None and "ANDROID_HOME" in os.environ:
      sdkm = distutils.spawn.find_executable(
          os.path.join(os.environ["ANDROID_HOME"], "tools", "bin",
          "sdkmanager"))

    elif sdkm is None and "ANDROID_HOME" not in os.environ:
      def sdkpath(p):
        return os.path.realpath(os.path.join(self.installto, "android-sdk", p))

      if not os.path.isdir(os.path.join(self.installto, "android-sdk")):
        def instpath(p):
          return os.path.realpath(os.path.join(self.installto, p))
        sdkhome = os.path.join(self.installto, "android-sdk")
        pathcmd = "PATH=$PATH:%s" % ":".join([s for s in [
            os.path.join("$ANDROID_HOME", "tools", "bin"),
            "$ANDROID_NDK_HOME" ]])
        print("Android SDK appears to be missing. (Based on the missing")
        print("environment variables below.) I will download it...")
        print("Hit CTRL + C if you just forgot to setup:")
        print("    ANDROID_HOME=%s" % sdkpath(""))
        print("    ANDROID_NDK_HOME=%s" % sdkpath("ndk-bundle"))
        print("    %s" % pathcmd)
        print("")
        fileset = []
        self.get_sdk_tools(fileset)
        for i in range(len(fileset)):
          fileset[i]["local"] = instpath(fileset[i]["url"].split("/")[-1])
          self.download_file(fileset, i)

        saved_cwd = os.getcwd()
        os.chdir(self.installto)
        os.mkdir("android-sdk")
        os.chdir("android-sdk")
        for i in range(len(fileset)):
          zipfile = fileset[i]["url"].split("/")[-1]
          print("unzip %s" % zipfile)
          runcmd_ignore_output(os.environ.copy()).run(
            [ "unzip", "-q", os.path.join("..", zipfile) ])
        os.rename(zipfile[:16], "ndk-bundle")

        sdkm = os.path.join("tools", "bin", "sdkmanager")
        self.get_sdkm_list_max(sdkm)
        print("")
        print("You can now review and agree to the EULA for the Android SDK.")
        sys.stdout.write("Continue with license review? (Y/n): ")
        sys.stdout.flush()
        python3_input = None
        try:
          # attempt the python2 form
          python3_input = raw_input
        except NameError:
          # attempt the python3 form
          python3_input = input
        line = python3_input()
        if line != "" and line != "y" and line != "Y":
          print(" you typed \"%s\". exiting." % line)
          sys.exit(1)

        for p in [ "build-tools;" + self.max_buildtools,
                    "platforms;android-%s" % self.max_android_sdk ]:
          self.run_sdkm(sdkm, ["--verbose", p], ignore_output = True)
        self.run_sdkm(sdkm, ["--licenses"], ignore_output = True)
        print("")
        print("Now is a good time to edit .bashrc / .cshrc / .zshrc and set:")
        print("    ANDROID_HOME=%s" % sdkpath(""))
        print("    ANDROID_NDK_HOME=%s" % sdkpath("ndk-bundle"))
        print("    %s" % pathcmd)
        print("")
        os.chdir(saved_cwd)
      else:
        print("ok, pretending ANDROID_HOME=%s" % sdkpath(""))

      os.environ["ANDROID_HOME"] = sdkpath("")
      os.environ["ANDROID_NDK_HOME"] = sdkpath("ndk-bundle")

    elif "ANDROID_HOME" not in os.environ:
      p = sdkm.split(os.sep)
      if p[0] == "":
        p[0] = os.sep
      p = p[:-3] + ["build-tools"]
      if os.path.isdir(os.path.join(*p)):
        p = p[:-1]
        os.environ["ANDROID_HOME"] = os.path.join(*p)
        print("ok, pretending ANDROID_HOME=%s" % os.path.join(*p))
      else:
        print("found %s but not ANDROID_HOME" % sdkm)
        sys.exit(1)

    # Now ANDROID_HOME and sdkm are correctly set.
    sdk_home = os.environ["ANDROID_HOME"]
    if not os.path.isdir(os.path.join(sdk_home, "build-tools")):
      print("found ANDROID_HOME but no \"build-tools\" dir in it")
      print("Hint: unless this dir is your pre-existing android SDK,")
      print("      you should delete it and \"unset ANDROID_HOME\" to proceed:")
      print("      %s" % sdk_home)
      sys.exit(1)

    if "ANDROID_NDK_HOME" not in os.environ:
      maybe_ndk = os.path.join(sdk_home, "ndk-bundle")
      if os.path.isdir(maybe_ndk):
        os.environ["ANDROID_NDK_HOME"] = maybe_ndk
      else:
        # Do not automatically install NDK. User set up their own Android SDK.
        print("found ANDROID_HOME but not ANDROID_NDK_HOME")
        print("Please type:   sdkmanager --verbose ndk-bundle")
        sys.exit(1)

    # detect buildtools version
    sdk_build_tools = os.path.join(sdk_home, "build-tools")
    max_ver = ""
    for v in os.listdir(sdk_build_tools):
      if (os.path.isdir(os.path.join(sdk_build_tools, v)) and
          distutils.spawn.find_executable(
              os.path.join(sdk_build_tools, v, "zipalign")) is not None):
        max_ver = max(v, max_ver)
    if max_ver == "":
      print("found ANDROID_HOME and build-tools dir in it,")
      print("but nothing is installed")
      sys.exit(1)
    self.cur_buildtools = max_ver

    # detect platforms/android-NN
    platform = os.path.join(sdk_home, "platforms")
    max_ver = 0
    for v in os.listdir(platform):
      if (v[:8] == "android-" and
          os.access(os.path.join(platform, v, "android.jar"), os.R_OK) and
          v[8:].isdigit()):
        max_ver = max(int(v[8:]), max_ver)
    if max_ver == 0:
      print("found ANDROID_HOME and platforms dir in it,")
      print("but nothing is installed")
      sys.exit(1)
    self.cur_android_sdk = max_ver
    if max_ver < minsdk:
      print("found %s/android-%s, wanted at least android-%d" % (
            platform, max_ver, minsdk))
      sys.exit(1)


# android_builder extends android_environ and adds its own abilities:
# 1. Customize the build according to command line arguments.
# 2. Runs build.cmd to get access to gn
# 3. Runs gn args to configure the android build
# 4. Runs ninja to build the target
class android_builder(android_environ):
  def __init__(self):
    super(android_builder, self).__init__()

  def run(self):
    git_path = git_rev_parse(["--show-prefix"])
    if not git_path:
      # TODO: it may be useful in the future to get the app name from BUILD.gn
      print("Android app is named by looking at the current dir")
      print("Please cd into a subdir before running %s" % sys.argv[0])
      sys.exit(1)
    if git_path[-1] == "/":
      git_path = git_path[0:len(git_path) - 1]
    (git_prefix, app_name) = os.path.split(git_path)

    # Generate a self-signed key used to run local APKs on a local device.
    keystore_file = os.path.join(os.path.expanduser("~"), ".android",
                                 "debug.keystore")
    if not os.path.isfile(keystore_file):
      keystore_pass = "android"
      runcmd_ignore_output(os.environ.copy()).run([
        "keytool", "-genkeypair", "-v",
        "-keystore", keystore_file,
        "-alias", "androiddebugkey",
        "-keypass", keystore_pass,
        "-storepass", keystore_pass,
        "-keyalg", "RSA",
        "-validity", "365000",
        "-dname", "cn=Android Debug,o=Android,c=US",
      ])

    android_variants = [
      #"arm", breaks in vulkanmemoryallocator
      "arm64",
      #"mipsel", mipsel broken on NDK r16 and later
      #"mips64el", mips64el broken on NDK r16 and later
      #"x86", breaks in vulkanmemoryallocator
      "x64",
    ]
    def target_validator(s):
      for arch in s.split(","):
        if arch not in android_variants:
          raise argparse.ArgumentTypeError("%s invalid (default: \"%s\")" % (
                                          arch, ",".join(android_variants)))
      return s

    parser = argparse.ArgumentParser(description="Build an .apk file.")
    parser.add_argument("-target", action="append", type=target_validator)
    parsed_args = parser.parse_args()
    wanted = ["\"%s\"" % v for v in android_variants]
    if parsed_args.target is not None:
      wanted = ["\"%s\"" % v for v in ",".join(parsed_args.target).split(",")]

    # Vulkan is in android-24 and all later versions (Android 7.0 Nougat)
    # AAudio is in android-26 and all later versions (Android 8.0 Oreo)
    minsdk = 26

    self.find_android_sdk(minsdk)

    # Run build.cmd. Bail out if its return code is not 0.
    cenv = os.environ.copy()
    cenv["VOLCANOSAMPLES_NO_NINJA"] = "1"
    runcmd_ignore_output(cenv).run(
      [ "bash", "-c", os.path.join(git_toplevel, "build.cmd") ])

    # Run gn args to tell it where to find the android SDK
    args = {}
    #args["android_variants_dir"] = libs_dir
    args["android_sdk"] = "\"%s\"" % os.environ["ANDROID_HOME"]
    args["android_sdk_version"] = "\"%d\"" % minsdk
    args["android_sdk_build_tools_version"] = "\"%s\"" % self.cur_buildtools
    args["android_variants"] = "[%s]" % ",".join(wanted)
    args["android_latest_sdk"] = "%d" % self.cur_android_sdk
    args["android_keystore_file"] = "\"%s\"" % keystore_file
    gn_path = os.path.join(git_toplevel, "vendor", "subgn")
    ninja_dir = os.path.join(git_toplevel, "out", "Debug")
    with open(os.path.join(ninja_dir, "args.gn"), "a+") as argfile:
      argfile.seek(0)
      keep = []
      for line in argfile:
        linekv = line.split("=")
        k = linekv[0].strip()
        if k not in args:
          # only keep the lines that are not being overridden
          keep.append(linekv)
      argfile.seek(0)
      argfile.truncate()
      # write keep back to argfile. keep is in the same order as argfile was.
      for a in keep:
        if len(a) == 1:
          argfile.write("%s\n" % a[0].strip())
        else:
          argfile.write("%s = %s\n" % (a[0].strip(), a[1].strip()))
      # write args to argfile in whatever order python chooses.
      for k in args:
        argfile.write("%s = %s\n" % (k, args[k]))

    runcmd_ignore_output(os.environ.copy()).run(
      [ os.path.join(gn_path, "gn"), "gen", ninja_dir ])

    ninja_target = "%s:android-%s" % (git_path, app_name)
    if "VOLCANOSAMPLES_NO_NINJA" not in os.environ:
      runcmd_ignore_output(os.environ.copy()).run(
        [ os.path.join(gn_path, "ninja"), "-C", ninja_dir,
          ninja_target ])

if __name__ == "__main__":
  builder = android_builder()
  builder.run()
