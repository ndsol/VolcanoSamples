#!/bin/bash
# Copyright (c) 2017-2018 the Volcano Authors. Licensed under GPLv3.

set -e

SAVED_PWD="$PWD"
cd -P -- "$(dirname -- "$0")"
cd .. # build-posix.sh is in src, cd to repo root (without using git)

git submodule update --init
mkdir -p out
export VOLCANO_ARGS_REQUEST="$PWD/out/volcano-args.txt"
VOLCANO_NO_OUT=1 vendor/volcano/build.cmd
if [ -f "$VOLCANO_ARGS_REQUEST" ]; then
  volcanoargs=$(cat "$VOLCANO_ARGS_REQUEST")
else
  echo "BUG: vendor/volcano/build.cmd ignored VOLCANO_ARGS_REQUEST"
  exit 1
fi
unset VOLCANO_ARGS_REQUEST

TARGET=Debug
NINJA_TARGET=""

# TODO: This code is no longer used. See its replacement at:
# vendor/volcano/src/gn/toolchain/android/bundle.py
if [ "$V_ANDROID_MINSDK" != "" -o "$V_ANDROID_LATESTSDK" != "" \
      -o "$V_ANDROID_BUILDTOOLS" != "" \
      -o "$V_ADD_TO_APPLICATION" != "" -o "$V_ADD_TO_ACTIVITY" != "" \
      -o "$V_ANDROID_VARIANTS" != "" ]; then
  if [ "$V_ANDROID_MINSDK" == "" -o "$V_ANDROID_LATESTSDK" == "" \
       -o "$V_ANDROID_BUILDTOOLS" == "" \
       -o "$V_ADD_TO_APPLICATION" == "" -o "$V_ADD_TO_ACTIVITY" == "" ]; then
    echo "ANDROID_HOME=$ANDROID_HOME"
    echo "ANDROID_NDK_HOME=$ANDROID_NDK_HOME"
    echo "V_ANDROID_MINSDK=$V_ANDROID_MINSDK"
    echo "V_ANDROID_LATESTSDK=$V_ANDROID_LATESTSDK"
    echo "V_ANDROID_BUILDTOOLS=$V_ANDROID_BUILDTOOLS"
    echo "V_ADD_TO_APPLICATION=$V_ADD_TO_APPLICATION"
    echo "V_ADD_TO_ACTIVITY=$V_ADD_TO_ACTIVITY"
    echo "V_ANDROID_VARIANTS=$V_ANDROID_VARIANTS"
    echo "All must be set; build-android.py does this all for you."
    if [ -z "$HOME" ]; then
      echo "The \$HOME environment variable is missing. Are you logged in?"
    fi
    exit 1
  fi
  if [ -z "$HOME" ]; then
    echo "The \$HOME environment variable is missing. Are you logged in?"
    exit 1
  fi

  BUILD_PWD="$PWD"
  cd "$SAVED_PWD"
  git_toplevel=$(git rev-parse --show-toplevel)
  libpathname=$(git rev-parse --show-prefix)
  if [ ! -d "$git_toplevel/$libpathname" ]; then
    echo "Invalid git_toplevel=$git_toplevel"
    echo "or      prefix=$libpathname"
    exit 1
  fi
  # The libname is only the last directory
  libname="$(basename ${libpathname})" # Remove trailing slash.
  libpathname="${libpathname%/}" # Remove trailing slash.
  NINJA_TARGET="${libpathname}:droid-all-variants-${libname}"

  dest_dir="$git_toplevel/out/$TARGET"
  app_dir="gen/$libname/app"
  libs_dir="$app_dir/build/intermediates/cmake/debug/obj"
  mkdir -p "$dest_dir/$libs_dir"
  HAVE_M=""
  HAVE_ARCH=""
  for f in $(find "$dest_dir/$libs_dir" -name add-to-manifest.txt); do
    M=$(cat "$f")
    if [ -z "$M" ]; then
      continue
    fi
    arch="${f#$dest_dir/$libs_dir/}"
    arch="${arch%%/*}"
    if [ -z "$HAVE_M" ]; then
      HAVE_M="$M"
      HAVE_ARCH="$arch"
      continue
    fi
    if [ "$HAVE_M" != "$M" ]; then
      echo "AndroidManifest.xml for $HAVE_ARCH differs from $arch."
      echo "$HAVE_ARCH AndroidManifest.xml:"
      echo "$HAVE_M"
      echo "$arch AndroidManifest.xml:"
      echo "$M"
      echo ""
      echo "Check your permission() rules. If you do want permissions to"
      echo "vary per-arch, then you must build a separate APK for each arch."
      exit 1
    fi
  done
  # $HAVE_M is valid. It is inserted in the AndroidManifest.xml heredoc, below.

  echo "extract V_ANDROID_CLASS from add-to-app dirs"
  exit 1
  V_ADD_TO_APPLICATION="
    android:label=\"VolcanoSample\"
    android:theme=\"@android:style/Theme.Translucent.NoTitleBar\"
"
  # screenOrientation="fullUser":
  # can use fullSensor to rotate even if user sets the rotation-lock.
  configChanges=""
  for c in keyboardHidden keyboard orientation screenSize \
           screenLayout smallestScreenSize locale layoutDirection \
           uiMode; do
    configChanges="${configChanges}${c}|"
  done
  configChanges="${configChanges%|}"
  V_ADD_TO_ACTIVITY="
    android:label="VolcanoSample"
    android:screenOrientation="fullUser"
    android:configChanges="$configChanges"
"

  # Write files
  cd "$dest_dir/gen/$libname"

  mkdir -p gradle/wrapper
  echo "# Project-wide Gradle settings." >gradle.properties
  echo "include ':app'" >settings.gradle
  echo "# Add project specific ProGuard rules here." >app/proguard-rules.pro

  echo "sdk.dir=$ANDROID_HOME" >local.properties
  echo "ndk.dir=$ANDROID_NDK_HOME" >>local.properties

  cat <<EOF >gradle/wrapper/gradle-wrapper.properties
  distributionBase=GRADLE_USER_HOME
  distributionPath=wrapper/dists
  zipStoreBase=GRADLE_USER_HOME
  zipStorePath=wrapper/dists
  distributionUrl=https\://services.gradle.org/distributions/gradle-3.3-all.zip
EOF
  mkdir -p app/src/main
  cat <<EOF >app/src/main/AndroidManifest.xml
<?xml version="1.0"?>
  <!--
  There are bugs building with Android Studio.
  Currently, the native library gets dropped by Android Studio unless the
  following command is run *after* opening the project, but *before*
  doing a "Build APK" in Android Studio:

  cd ../out
  for arch in arm64-v8a armeabi-v7a x86 x86_64; do (
    cd .externalNativeBuild/cmake/debug/\$arch && \
    ~/src/android-sdk/cmake/3*/bin/ninja -v
  ); done
  -->
  <manifest xmlns:android="http://schemas.android.com/apk/res/android"
      package="$V_ANDROID_CLASS"
      android:versionCode="1"
      android:versionName="0.1">

$HAVE_M
      <uses-permission android:name="android.permission.SET_DEBUG_APP"/>
      <uses-feature android:name="android.software.leanback" android:required="false" />
      <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
      <uses-feature android:name="android.hardware.gamepad" android:required="false"/>
      <uses-sdk android:minSdkVersion="$V_ANDROID_MINSDK"
                android:targetSdkVersion="$V_ANDROID_LATESTSDK"/>
      <application
        android:hasCode="false"
$V_ADD_TO_APPLICATION
        android:debuggable="true">
          <activity android:name="android.app.NativeActivity"
$V_ADD_TO_ACTIVITY
              android:exported="true">

              <!-- pass name of .so to NativeActivity -->
              <meta-data android:name="android.app.lib_name" android:value="${libname}"/>
              <intent-filter>
                  <action android:name="android.intent.action.MAIN"/>
                  <category android:name="android.intent.category.LAUNCHER"/>
                  <category android:name="android.intent.category.LEANBACK_LAUNCHER"/>
              </intent-filter>
          </activity>
      </application>
  </manifest>
EOF
  cat <<EOF >build.gradle
  buildscript {
      repositories {
          jcenter()
      }
      dependencies {
          classpath 'com.android.tools.build:gradle:2.3.1'

          // NOTE: Do not place your application dependencies here; they
          // belong in the individual module build.gradle files
      }
  }

  allprojects {
      repositories {
          jcenter()
      }
  }

  task clean(type: Delete) {
      delete rootProject.buildDir
  }
EOF
  cat <<EOF >app/build.gradle
  apply plugin: 'com.android.application'

  android {
    compileSdkVersion $V_ANDROID_MINSDK
    buildToolsVersion '$V_ANDROID_BUILDTOOLS'

    defaultConfig {
      applicationId  "$V_ANDROID_CLASS"
      minSdkVersion    $V_ANDROID_MINSDK
      targetSdkVersion $V_ANDROID_LATESTSDK
      versionCode 1
      versionName "0.0.1"

      // Shader compilation directives, put glsl shaders to app/src/main/shaders
      // android studio will pick them up and compile them into APK/assets/shaders
      // app/src/main/shaders is a symlink.
      //
      // KNOWN ISSUE:  if shaders having errors, it takes long time for gradle to timeout
      //               but it will eventually time out and complain about shader compiling
      shaders {
        glslcArgs.addAll(['-c', '-g'])
      }
      // Compile native code using CMakeLists.txt
      externalNativeBuild {
        cmake {
            abiFilters 'armeabi-v7a', 'arm64-v8a', 'x86', 'x86_64'
            arguments '-DANDROID_TOOLCHAIN=clang', '-DANDROID_STL=gnustl_static'
        }
      }
    }
    externalNativeBuild {
      cmake {
        path 'CMakeLists.txt'
      }
    }

    buildTypes {
      release {
        minifyEnabled = false
        proguardFiles getDefaultProguardFile('proguard-android.txt'), 'proguard-rules.pro'
      }
    }
  }
EOF
  cat <<EOF >app/CMakeLists.txt
cmake_minimum_required(VERSION 2.8)
add_custom_target(astudio-buildstep ALL
                  bash -v ../astudio-buildstep.sh \${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/lib03android.so
                  WORKING_DIRECTORY \${CMAKE_HOME_DIRECTORY})
set(APP_GLUE_DIR \${ANDROID_NDK}/sources/android/native_app_glue)
include_directories(\${APP_GLUE_DIR})
add_library(app-glue STATIC \${APP_GLUE_DIR}/android_native_app_glue.c)
target_link_libraries(app-glue log android)
EOF
  cat <<EOF >astudio-buildstep.sh
#!/bin/bash
../vendor/subgn/ninja "\${1#\$PWD/}"
EOF
  B64DEC="base64 -d"
  if [ "$(uname)" == "Darwin" ]; then
    B64DEC="base64 -D"
  fi
  $B64DEC <<EOF >gradle/wrapper/gradle-wrapper.jar
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
aWVzU87NTynNSVVISU3LzMssyczP41IOyShVcCwoUjAwUTAwtzIytjIxVnB2DQ5RMDIwNOYqKMrP
Sk0uKbblKirNK8nMTbXlAgBQSwECFAMKAAAICAANO4RCAAAAAAIAAAAAAAAACQAAAAAAAAAAABAA
7UEAAAAATUVUQS1JTkYvUEsBAhQDCgAACAgADTuEQtO44BFOAAAAaAAAABQAAAAAAAAAAAAAAKSB
KQAAAE1FVEEtSU5GL01BTklGRVNULk1GUEsBAhQDCgAACAgACzuEQgAAAAACAAAAAAAAAAQAAAAA
AAAAAAAQAO1BqQAAAG9yZy9QSwECFAMKAAAICAALO4RCAAAAAAIAAAAAAAAACwAAAAAAAAAAABAA
7UHNAAAAb3JnL2dyYWRsZS9QSwECFAMKAAAICAALO4RCAAAAAAIAAAAAAAAAEwAAAAAAAAAAABAA
7UH4AAAAb3JnL2dyYWRsZS93cmFwcGVyL1BLAQIUAwoAAAgIAAs7hEJogmRmowAAANUAAAAjAAAA
AAAAAAAAAACkgSsBAABvcmcvZ3JhZGxlL3dyYXBwZXIvRG93bmxvYWQkMS5jbGFzc1BLAQIUAwoA
AAgIAAs7hEIKvM6AFgIAAHAEAABEAAAAAAAAAAAAAACkgQ8CAABvcmcvZ3JhZGxlL3dyYXBwZXIv
RG93bmxvYWQkU3lzdGVtUHJvcGVydGllc1Byb3h5QXV0aGVudGljYXRvci5jbGFzc1BLAQIUAwoA
AAgIAAs7hELn7FhzqgAAANsAAAAiAAAAAAAAAAAAAACkgYcEAABvcmcvZ3JhZGxlL3dyYXBwZXIv
SURvd25sb2FkLmNsYXNzUEsBAhQDCgAACAgACzuEQttIRSoFBgAAUAsAADMAAAAAAAAAAAAAAKSB
cQUAAG9yZy9ncmFkbGUvd3JhcHBlci9FeGNsdXNpdmVGaWxlQWNjZXNzTWFuYWdlci5jbGFzc1BL
AQIUAwoAAAgIAAs7hEJjr4G6YAIAABAGAAAtAAAAAAAAAAAAAACkgccLAABvcmcvZ3JhZGxlL3dy
YXBwZXIvV3JhcHBlckNvbmZpZ3VyYXRpb24uY2xhc3NQSwECFAMKAAAICAALO4RCfyGmrOYEAAAZ
CgAAMAAAAAAAAAAAAAAApIFyDgAAb3JnL2dyYWRsZS93cmFwcGVyL1N5c3RlbVByb3BlcnRpZXNI
YW5kbGVyLmNsYXNzUEsBAhQDCgAACAgACzuEQitObQkaBwAAcQ4AACYAAAAAAAAAAAAAAKSBphMA
AG9yZy9ncmFkbGUvd3JhcHBlci9QYXRoQXNzZW1ibGVyLmNsYXNzUEsBAhQDCgAACAgACzuEQuC3
KC/yDgAAnSAAACAAAAAAAAAAAAAAAKSBBBsAAG9yZy9ncmFkbGUvd3JhcHBlci9JbnN0YWxsLmNs
YXNzUEsBAhQDCgAACAgACzuEQkfcGsTCBAAAlQkAAC0AAAAAAAAAAAAAAKSBNCoAAG9yZy9ncmFk
bGUvd3JhcHBlci9Cb290c3RyYXBNYWluU3RhcnRlci5jbGFzc1BLAQIUAwoAAAgIAAs7hEJmW9vx
QQoAALUWAAAoAAAAAAAAAAAAAACkgUEvAABvcmcvZ3JhZGxlL3dyYXBwZXIvV3JhcHBlckV4ZWN1
dG9yLmNsYXNzUEsBAhQDCgAACAgACzuEQnt3YH4lCgAADhYAACoAAAAAAAAAAAAAAKSByDkAAG9y
Zy9ncmFkbGUvd3JhcHBlci9HcmFkbGVXcmFwcGVyTWFpbi5jbGFzc1BLAQIUAwoAAAgIAAs7hEJP
3TYEFgYAANEMAAAiAAAAAAAAAAAAAACkgTVEAABvcmcvZ3JhZGxlL3dyYXBwZXIvSW5zdGFsbCQx
LmNsYXNzUEsBAhQDCgAACAgACzuEQhqIgPyyAQAAVgMAADgAAAAAAAAAAAAAAKSBi0oAAG9yZy9n
cmFkbGUvd3JhcHBlci9QYXRoQXNzZW1ibGVyJExvY2FsRGlzdHJpYnV0aW9uLmNsYXNzUEsBAhQD
CgAACAgACzuEQkl9yCM4BwAAeg0AACEAAAAAAAAAAAAAAKSBk0wAAG9yZy9ncmFkbGUvd3JhcHBl
ci9Eb3dubG9hZC5jbGFzc1BLAQIUAwoAAAgIAAs7hEJVCUhQUQAAAE8AAAAjAAAAAAAAAAAAAACk
gQpUAABncmFkbGUtd3JhcHBlci1jbGFzc3BhdGgucHJvcGVydGllc1BLAQIUAwoAAAgIAPU6hEKI
JHgUygAAAB0BAAAYAAAAAAAAAAAAAACkgZxUAABidWlsZC1yZWNlaXB0LnByb3BlcnRpZXNQSwEC
FAMKAAAICAD1OoRCAAAAAAIAAAAAAAAADwAAAAAAAAAAABAA7UGcVQAAb3JnL2dyYWRsZS9jbGkv
UEsBAhQDCgAACAgA9TqEQoju8mDnAgAAkAcAADEAAAAAAAAAAAAAAKSBy1UAAG9yZy9ncmFkbGUv
Y2xpL0Fic3RyYWN0Q29tbWFuZExpbmVDb252ZXJ0ZXIuY2xhc3NQSwECFAMKAAAICAD1OoRCMl9l
j6YAAADoAAAAKAAAAAAAAAAAAAAApIEBWQAAb3JnL2dyYWRsZS9jbGkvQ29tbWFuZExpbmVQYXJz
ZXIkMS5jbGFzc1BLAQIUAwoAAAgIAPU6hEIKgruqoQIAAPcGAAA8AAAAAAAAAAAAAACkge1ZAABv
cmcvZ3JhZGxlL2NsaS9Db21tYW5kTGluZVBhcnNlciRNaXNzaW5nT3B0aW9uQXJnU3RhdGUuY2xh
c3NQSwECFAMKAAAICAD1OoRCHoMKl5MCAACDBQAAPQAAAAAAAAAAAAAApIHoXAAAb3JnL2dyYWRs
ZS9jbGkvQ29tbWFuZExpbmVQYXJzZXIkT3B0aW9uU3RyaW5nQ29tcGFyYXRvci5jbGFzc1BLAQIU
AwoAAAgIAPU6hEKXm8jLRgEAAEsCAAAxAAAAAAAAAAAAAACkgdZfAABvcmcvZ3JhZGxlL2NsaS9D
b21tYW5kTGluZUFyZ3VtZW50RXhjZXB0aW9uLmNsYXNzUEsBAhQDCgAACAgA9TqEQmxaNbaEBwAA
+xIAAD0AAAAAAAAAAAAAAKSBa2EAAG9yZy9ncmFkbGUvY2xpL0NvbW1hbmRMaW5lUGFyc2VyJEtu
b3duT3B0aW9uUGFyc2VyU3RhdGUuY2xhc3NQSwECFAMKAAAICAD1OoRC/5B8ENMCAADbBgAANwAA
AAAAAAAAAAAApIFKaQAAb3JnL2dyYWRsZS9jbGkvQ29tbWFuZExpbmVQYXJzZXIkT3B0aW9uQ29t
cGFyYXRvci5jbGFzc1BLAQIUAwoAAAgIAPU6hELhgJ553wIAAG4HAAA/AAAAAAAAAAAAAACkgXJs
AABvcmcvZ3JhZGxlL2NsaS9Db21tYW5kTGluZVBhcnNlciRVbmtub3duT3B0aW9uUGFyc2VyU3Rh
dGUuY2xhc3NQSwECFAMKAAAICAD1OoRCSfyLxfoEAACTCwAAJgAAAAAAAAAAAAAApIGubwAAb3Jn
L2dyYWRsZS9jbGkvQ29tbWFuZExpbmVPcHRpb24uY2xhc3NQSwECFAMKAAAICAD1OoRC+tbQPqYB
AACmAwAAOAAAAAAAAAAAAAAApIHsdAAAb3JnL2dyYWRsZS9jbGkvQ29tbWFuZExpbmVQYXJzZXIk
T3B0aW9uUGFyc2VyU3RhdGUuY2xhc3NQSwECFAMKAAAICAD1OoRCg5SGtfQGAAAWEAAAJgAAAAAA
AAAAAAAApIHodgAAb3JnL2dyYWRsZS9jbGkvUGFyc2VkQ29tbWFuZExpbmUuY2xhc3NQSwECFAMK
AAAICAD1OoRCi0E1bHwBAAALAwAAOgAAAAAAAAAAAAAApIEgfgAAb3JnL2dyYWRsZS9jbGkvUHJv
amVjdFByb3BlcnRpZXNDb21tYW5kTGluZUNvbnZlcnRlci5jbGFzc1BLAQIUAwoAAAgIAPU6hEIx
DKmrUAIAAAQFAABGAAAAAAAAAAAAAACkgfR/AABvcmcvZ3JhZGxlL2NsaS9Db21tYW5kTGluZVBh
cnNlciRDYXNlSW5zZW5zaXRpdmVTdHJpbmdDb21wYXJhdG9yLmNsYXNzUEsBAhQDCgAACAgA9TqE
QqJWmCKXEAAA3CYAACYAAAAAAAAAAAAAAKSBqIIAAG9yZy9ncmFkbGUvY2xpL0NvbW1hbmRMaW5l
UGFyc2VyLmNsYXNzUEsBAhQDCgAACAgA9TqEQpBg98WlAgAAKQcAADMAAAAAAAAAAAAAAKSBg5MA
AG9yZy9ncmFkbGUvY2xpL0NvbW1hbmRMaW5lUGFyc2VyJEFmdGVyT3B0aW9ucy5jbGFzc1BLAQIU
AwoAAAgIAPU6hEJRxmKMlwIAAJwFAAAzAAAAAAAAAAAAAACkgXmWAABvcmcvZ3JhZGxlL2NsaS9D
b21tYW5kTGluZVBhcnNlciRPcHRpb25TdHJpbmcuY2xhc3NQSwECFAMKAAAICAD1OoRC/blFI7EE
AAAhDAAAOwAAAAAAAAAAAAAApIFhmQAAb3JnL2dyYWRsZS9jbGkvQWJzdHJhY3RQcm9wZXJ0aWVz
Q29tbWFuZExpbmVDb252ZXJ0ZXIuY2xhc3NQSwECFAMKAAAICAD1OoRCk528FbwCAAAgBQAALAAA
AAAAAAAAAAAApIFrngAAb3JnL2dyYWRsZS9jbGkvUGFyc2VkQ29tbWFuZExpbmVPcHRpb24uY2xh
c3NQSwECFAMKAAAICAD1OoRCN8mKr5kCAACqBgAAPQAAAAAAAAAAAAAApIFxoQAAb3JnL2dyYWRs
ZS9jbGkvQ29tbWFuZExpbmVQYXJzZXIkT3B0aW9uQXdhcmVQYXJzZXJTdGF0ZS5jbGFzc1BLAQIU
AwoAAAgIAPU6hEIkuFL8PAEAAFkDAAApAAAAAAAAAAAAAACkgWWkAABvcmcvZ3JhZGxlL2NsaS9D
b21tYW5kTGluZUNvbnZlcnRlci5jbGFzc1BLAQIUAwoAAAgIAPU6hEJAzaQx6QMAAHwLAAA8AAAA
AAAAAAAAAACkgeilAABvcmcvZ3JhZGxlL2NsaS9Db21tYW5kTGluZVBhcnNlciRCZWZvcmVGaXJz
dFN1YkNvbW1hbmQuY2xhc3NQSwECFAMKAAAICAD1OoRCkwA7mXwBAAD8AgAAOQAAAAAAAAAAAAAA
pIErqgAAb3JnL2dyYWRsZS9jbGkvU3lzdGVtUHJvcGVydGllc0NvbW1hbmRMaW5lQ29udmVydGVy
LmNsYXNzUEsBAhQDCgAACAgA9TqEQpyzSX8DAgAAtwQAADIAAAAAAAAAAAAAAKSB/qsAAG9yZy9n
cmFkbGUvY2xpL0NvbW1hbmRMaW5lUGFyc2VyJFBhcnNlclN0YXRlLmNsYXNzUEsBAhQDCgAACAgA
9TqEQo82nDc8AwAAygkAADsAAAAAAAAAAAAAAKSBUa4AAG9yZy9ncmFkbGUvY2xpL0NvbW1hbmRM
aW5lUGFyc2VyJEFmdGVyRmlyc3RTdWJDb21tYW5kLmNsYXNzUEsBAhQDCgAACAgA9TqEQvaEjsJH
AAAARQAAAB8AAAAAAAAAAAAAAKSB5rEAAGdyYWRsZS1jbGktY2xhc3NwYXRoLnByb3BlcnRpZXNQ
SwUGAAAAAC8ALwBoEAAAarIAAAAA
EOF
    cat <<EOF >gradlew
#!/usr/bin/env bash
# Add default JVM options here. You can also use JAVA_OPTS and GRADLE_OPTS to pass JVM options to this script.
DEFAULT_JVM_OPTS=""

APP_NAME="Gradle"
APP_BASE_NAME=\`basename "\$0"\`

# Use the maximum available, or set MAX_FD != -1 to use that value.
MAX_FD="maximum"

warn ( ) {
    echo "\$*"
}

die ( ) {
    echo
    echo "\$*"
    echo
    exit 1
}

# OS specific support (must be 'true' or 'false').
cygwin=false
msys=false
darwin=false
case "\`uname\`" in
  CYGWIN* )
    cygwin=true
    ;;
  Darwin* )
    darwin=true
    ;;
  MINGW* )
    msys=true
    ;;
esac

# For Cygwin, ensure paths are in UNIX format before anything is touched.
if \$cygwin ; then
    [ -n "\$JAVA_HOME" ] && JAVA_HOME=\`cygpath --unix "\$JAVA_HOME"\`
fi

# Attempt to set APP_HOME
# Resolve links: \$0 may be a link
PRG="\$0"
# Need this for relative symlinks.
while [ -h "\$PRG" ] ; do
    ls=\`ls -ld "\$PRG"\`
    link=\`expr "\$ls" : '.*-> \\(.*\\)\$'\`
    if expr "\$link" : '/.*' > /dev/null; then
        PRG="\$link"
    else
        PRG=\`dirname "\$PRG"\`"/\$link"
    fi
done
SAVED="\`pwd\`"
cd "\`dirname \\"\$PRG\\"\`/" >&-
APP_HOME="\`pwd -P\`"
cd "\$SAVED" >&-

CLASSPATH=\$APP_HOME/gradle/wrapper/gradle-wrapper.jar

# Determine the Java command to use to start the JVM.
if [ -n "\$JAVA_HOME" ] ; then
    if [ -x "\$JAVA_HOME/jre/sh/java" ] ; then
        # IBM's JDK on AIX uses strange locations for the executables
        JAVACMD="\$JAVA_HOME/jre/sh/java"
    else
        JAVACMD="\$JAVA_HOME/bin/java"
    fi
    if [ ! -x "\$JAVACMD" ] ; then
        die "ERROR: JAVA_HOME is set to an invalid directory: \$JAVA_HOME

Please set the JAVA_HOME variable in your environment to match the
location of your Java installation."
    fi
else
    JAVACMD="java"
    which java >/dev/null 2>&1 || die "ERROR: JAVA_HOME is not set and no 'java' command could be found in your PATH.

Please set the JAVA_HOME variable in your environment to match the
location of your Java installation."
fi

# Increase the maximum file descriptors if we can.
if [ "\$cygwin" = "false" -a "\$darwin" = "false" ] ; then
    MAX_FD_LIMIT=\`ulimit -H -n\`
    if [ \$? -eq 0 ] ; then
        if [ "\$MAX_FD" = "maximum" -o "\$MAX_FD" = "max" ] ; then
            MAX_FD="\$MAX_FD_LIMIT"
        fi
        ulimit -n \$MAX_FD
        if [ \$? -ne 0 ] ; then
            warn "Could not set maximum file descriptor limit: \$MAX_FD"
        fi
    else
        warn "Could not query maximum file descriptor limit: \$MAX_FD_LIMIT"
    fi
fi

# For Darwin, add options to specify how the application appears in the dock
if \$darwin; then
    GRADLE_OPTS="\$GRADLE_OPTS \\"-Xdock:name=\$APP_NAME\\" \\"-Xdock:icon=\$APP_HOME/media/gradle.icns\\""
fi

# For Cygwin, switch paths to Windows format before running java
if \$cygwin ; then
    APP_HOME=\`cygpath --path --mixed "\$APP_HOME"\`
    CLASSPATH=\`cygpath --path --mixed "\$CLASSPATH"\`

    # We build the pattern for arguments to be converted via cygpath
    ROOTDIRSRAW=\`find -L / -maxdepth 1 -mindepth 1 -type d 2>/dev/null\`
    SEP=""
    for dir in \$ROOTDIRSRAW ; do
        ROOTDIRS="\$ROOTDIRS\$SEP\$dir"
        SEP="|"
    done
    OURCYGPATTERN="(^(\$ROOTDIRS))"
    # Add a user-defined pattern to the cygpath arguments
    if [ "\$GRADLE_CYGPATTERN" != "" ] ; then
        OURCYGPATTERN="\$OURCYGPATTERN|(\$GRADLE_CYGPATTERN)"
    fi
    # Now convert the arguments - kludge to limit ourselves to /bin/sh
    i=0
    for arg in "\$@" ; do
        CHECK=\`echo "\$arg"|egrep -c "\$OURCYGPATTERN" -\`
        CHECK2=\`echo "\$arg"|egrep -c "^-"\`                                 ### Determine if an option

        if [ \$CHECK -ne 0 ] && [ \$CHECK2 -eq 0 ] ; then                    ### Added a condition
            eval \`echo args\$i\`=\`cygpath --path --ignore --mixed "\$arg"\`
        else
            eval \`echo args\$i\`="\\"\$arg\\""
        fi
        i=\$((i+1))
    done
    case \$i in
        (0) set -- ;;
        (1) set -- "\$args0" ;;
        (2) set -- "\$args0" "\$args1" ;;
        (3) set -- "\$args0" "\$args1" "\$args2" ;;
        (4) set -- "\$args0" "\$args1" "\$args2" "\$args3" ;;
        (5) set -- "\$args0" "\$args1" "\$args2" "\$args3" "\$args4" ;;
        (6) set -- "\$args0" "\$args1" "\$args2" "\$args3" "\$args4" "\$args5" ;;
        (7) set -- "\$args0" "\$args1" "\$args2" "\$args3" "\$args4" "\$args5" "\$args6" ;;
        (8) set -- "\$args0" "\$args1" "\$args2" "\$args3" "\$args4" "\$args5" "\$args6" "\$args7" ;;
        (9) set -- "\$args0" "\$args1" "\$args2" "\$args3" "\$args4" "\$args5" "\$args6" "\$args7" "\$args8" ;;
    esac
fi

# Split up the JVM_OPTS And GRADLE_OPTS values into an array, following the shell quoting and substitution rules
function splitJvmOpts() {
    JVM_OPTS=("\$@")
}
eval splitJvmOpts \$DEFAULT_JVM_OPTS \$JAVA_OPTS \$GRADLE_OPTS
JVM_OPTS[\${#JVM_OPTS[*]}]="-Dorg.gradle.appname=\$APP_BASE_NAME"

exec "\$JAVACMD" "\${JVM_OPTS[@]}" -classpath "\$CLASSPATH" org.gradle.wrapper.GradleWrapperMain "\$@"
EOF
    awk '{printf "%s\r\n", $0}' <<EOF >gradlew.bat
@if "%DEBUG%" == "" @echo off
if "%OS%"=="Windows_NT" setlocal

set DEFAULT_JVM_OPTS=

set DIRNAME=%~dp0
if "%DIRNAME%" == "" set DIRNAME=.
set APP_BASE_NAME=%~n0
set APP_HOME=%DIRNAME%

if defined JAVA_HOME goto findJavaFromJavaHome

set JAVA_EXE=java.exe
%JAVA_EXE% -version >NUL 2>&1
if "%ERRORLEVEL%" == "0" goto init

echo.
echo ERROR: JAVA_HOME is not set and no 'java' command could be found in your PATH.
echo.
echo Please set the JAVA_HOME variable in your environment to match the
echo location of your Java installation.

goto fail

:findJavaFromJavaHome
set JAVA_HOME=%JAVA_HOME:"=%
set JAVA_EXE=%JAVA_HOME%/bin/java.exe

if exist "%JAVA_EXE%" goto init

echo.
echo ERROR: JAVA_HOME is set to an invalid directory: %JAVA_HOME%
echo.
echo Please set the JAVA_HOME variable in your environment to match the
echo location of your Java installation.

goto fail

:init

if not "%OS%" == "Windows_NT" goto win9xME_args
if "%@eval[2+2]" == "4" goto 4NT_args

:win9xME_args
set CMD_LINE_ARGS=
set _SKIP=2

:win9xME_args_slurp
if "x%~1" == "x" goto execute

set CMD_LINE_ARGS=%*
goto execute

:4NT_args
set CMD_LINE_ARGS=%\$

:execute

set CLASSPATH=%APP_HOME%\\gradle\\wrapper\\gradle-wrapper.jar

"%JAVA_EXE%" %DEFAULT_JVM_OPTS% %JAVA_OPTS% %GRADLE_OPTS% "-Dorg.gradle.appname=%APP_BASE_NAME%" -classpath "%CLASSPATH%" org.gradle.wrapper.GradleWrapperMain %CMD_LINE_ARGS%

:end
if "%ERRORLEVEL%"=="0" goto mainEnd

:fail
if  not "" == "%GRADLE_EXIT_CONSOLE%" exit 1
exit /b 1

:mainEnd
if "%OS%"=="Windows_NT" endlocal

:omega
EOF

  cd "$ANDROID_NDK_HOME/sources/android/native_app_glue"
  # Add support for runOnUiThread via APP_CMD_MAIN_THREAD
  patch --no-backup-if-mismatch -sN -r - -p1 <<EOF >/dev/null || true
--- a/android_native_app_glue.c	2017-07-21 11:03:50.000000000 -0500
+++ b/android_native_app_glue.c	2017-12-26 20:23:49.077666636 -0600
@@ -30,7 +30,7 @@
 #define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, "threaded_app", __VA_ARGS__))
 
 /* For debug builds, always enable the debug traces in this library */
-#ifndef NDEBUG
+#if 0 /*was NDEBUG*/
 #  define LOGV(...)  ((void)__android_log_print(ANDROID_LOG_VERBOSE, "threaded_app", __VA_ARGS__))
 #else
 #  define LOGV(...)  ((void)0)
@@ -319,6 +319,9 @@
 }
 
 static void android_app_free(struct android_app* android_app) {
+    ALooper_release(android_app->mainThreadLooper);
+    close(android_app->mainThreadPipe[0]);
+    close(android_app->mainThreadPipe[1]);
     pthread_mutex_lock(&android_app->mutex);
     android_app_write_cmd(android_app, APP_CMD_DESTROY);
     while (!android_app->destroyed) {
@@ -420,6 +423,16 @@
     android_app_set_input((struct android_app*)activity->instance, NULL);
 }
 
+static int onMainThreadPipe(int fd, int events, void* instance) {
+    struct android_app* app = (struct android_app*)instance;
+    char msg;
+    read(fd, &msg, sizeof(msg));  // Read and discard msg.
+    if (app->onAppCmd) {
+        app->onAppCmd(app, APP_CMD_MAIN_THREAD);
+    }
+    return 1;  /* return 1 to continue receiving callbacks */
+}
+
 JNIEXPORT
 void ANativeActivity_onCreate(ANativeActivity* activity, void* savedState,
                               size_t savedStateSize) {
@@ -439,4 +452,11 @@
     activity->callbacks->onInputQueueDestroyed = onInputQueueDestroyed;
 
     activity->instance = android_app_create(activity, savedState, savedStateSize);
+
+    struct android_app* app = activity->instance;
+    app->mainThreadLooper = ALooper_forThread();
+    ALooper_acquire(app->mainThreadLooper);
+    pipe(app->mainThreadPipe);
+    ALooper_addFd(app->mainThreadLooper, app->mainThreadPipe[0], 0,
+                  ALOOPER_EVENT_INPUT, onMainThreadPipe, app);
 }
--- a/android_native_app_glue.h	2017-07-21 11:03:50.000000000 -0500
+++ b/android_native_app_glue.h	2017-12-26 19:48:56.729562431 -0600
@@ -182,6 +182,8 @@
     AInputQueue* pendingInputQueue;
     ANativeWindow* pendingWindow;
     ARect pendingContentRect;
+    ALooper* mainThreadLooper;
+    int mainThreadPipe[2];
 };
 
 enum {
@@ -309,6 +311,8 @@
      * and waiting for the app thread to clean up and exit before proceeding.
      */
     APP_CMD_DESTROY,
+
+    APP_CMD_MAIN_THREAD,
 };
 
 /**
EOF

  cd "$BUILD_PWD"
fi

mkdir -p out/$TARGET

if [ -f out/$TARGET/args.gn ] && \
    grep '^# end of volcano args$' -q out/$TARGET/args.gn; then
  : # vendor/subgn/gn gen has already run, skip it
else
  if [ -n "$volcanoargs" ]; then
    args="--args=$volcanoargs"
    vendor/subgn/gn gen out/$TARGET "$args"
  else
    vendor/subgn/gn gen out/$TARGET
  fi
  echo "# end of volcano args" >> out/$TARGET/args.gn
fi

if [ -z "$VOLCANOSAMPLES_NO_NINJA" ]; then
  # Building using all cores will fail in e.g. a VM with limited RAM
  SMALL_MEM=""
  if [ -f /proc/meminfo ]; then
    SMALL_MEM=$(awk 'BEGIN{m=0}
                     {if ($1 ~ "MemTotal:") m=$2}
                     END{if (m/1024/1024 < 7) print "small"}' /proc/meminfo)
  fi
  NINJA_J=""
  if [ "$SMALL_MEM" == "small" ]; then
    echo "reducing parallelism due to low memory: -j2"
    NINJA_J="-j2"
  fi
  vendor/subgn/ninja -C out/$TARGET $NINJA_J $NINJA_TARGET
fi

if [ "$V_ANDROID_CLASS" != "" ]; then
  cd "$dest_dir"
  raw_asset_dir="gen/$libname/raw_asset"
  mkdir -p "$raw_asset_dir"

  validation_dir="$ANDROID_NDK_HOME/sources/third_party/vulkan"
  validation_dir="$validation_dir/src/build-android/jniLibs"
  aapt_libs_dir="$app_dir/build/for-aapt"
  rm -rf "$aapt_libs_dir"
  mkdir -p "$aapt_libs_dir/lib"

  # copy validation_dir .so files
  if [ -z "$V_ANDROID_SKIP_VALIDATION_LAYERS" ]; then
    for a in "$libs_dir"/*; do
      arch="${a#$libs_dir/}"
      (
        cd "${validation_dir}"
        tar cf - "$arch"
      ) | tar xf - -C "$aapt_libs_dir/lib"
    done
  fi

  # copy libs_dir .so files
  (
    cd "$libs_dir"
    tar cf - $(find . -type f -name '*.so')
  ) | tar xf - -C "$aapt_libs_dir/lib"

  unaligned="gen/$libname/$libname-unaligned.apk"
  aligned="$libname.apk"

  echo -n "aapt: "
  "$ANDROID_HOME/build-tools/$V_ANDROID_BUILDTOOLS/aapt" package \
    -v \
    -f \
    -M "$app_dir/src/main/AndroidManifest.xml" \
    -I "$ANDROID_HOME/platforms/android-$V_ANDROID_LATESTSDK/android.jar" \
    -A "$raw_asset_dir" \
    -F "$unaligned" \
    "$aapt_libs_dir"
  echo ""

  keystore_file="$HOME/.android/debug.keystore"
  keystore_pass="android"
  if [ ! -f "$keystore_file" ]; then
    keytool -genkeypair -v -keystore "$keystore_file" -alias androiddebugkey \
      -keypass "$keystore_pass" -storepass "$keystore_pass" -keyalg RSA \
      -validity 365000 -dname "cn=Android Debug,o=Android,c=US"
  fi
  jarsigner -verbose -keystore "$keystore_file" -storepass "$keystore_pass" \
    -keypass "$keystore_pass" "$unaligned" "androiddebugkey"
  "$ANDROID_HOME/build-tools/$V_ANDROID_BUILDTOOLS/zipalign" -f 4 \
    "$unaligned" "$aligned"
fi

if ! grep -q "vendor/subgn" <<<"$PATH"; then
  echo "Note: please add the following to your .bashrc / .cshrc / .zshrc:"
  echo ""
  echo "      PATH=\$PATH:$PWD/vendor/subgn"
  echo ""
  echo "      Which makes \"ninja -C out/$TARGET\" work."
  echo "      This script does that, so another option is to just re-run me."
  echo "      Then PATH changes are optional."
fi
