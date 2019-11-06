#!/bin/bash
# Copyright (c) 2017-2018 the Volcano Authors. Licensed under GPLv3.

set -e

cd -P -- "$(dirname -- "$0")"
# sync-from-upstream.sh is in src, cd to vendor
cd ../vendor

#todo="sync-glfw sync-spirv-cross sync-assimp"
#todo="$todo sync-subninja"
# WARNING: subgn has moved to c++14, do not update todo="$todo sync-subgn"
#todo="$todo sync-skia"
#todo="$todo sync-vulkansamples"
#todo="$todo sync-assimp"
todo="$todo sync-cereal"
todo="$todo sync-vulkanmemoryallocator"

git_export() {
  mkdir -p "$1"
  (
    cd "$1"
    git clone --depth=1 "$2"
    rm -rf "$(basename $2)/.git"
  )
}

for item in $todo; do
  case $item in
  sync-subninja)
  (
    cd volcano/vendor/subgn
    R=$(git submodule status)
    while read hash name branch; do
      if [ -z "$hash" ]; then
        # ignore blank lines
        continue
      fi
      if [ "${branch#(heads/}" == "$branch" ]; then
        echo "Unable to parse output of git submodule status:"
        git submodule status
        exit 1
      fi
      branch="${branch#(heads/}"
      branch="${branch%)}"
      break
    done <<<"$R"
    (
      cd subninja
      R=$(git remote -v)
      while read r url_and_other_stuff; do
        if [ "$r" == "parent" ]; then
          break
        fi
      done <<<"$R"
      if [ -z "$r" ]; then
        echo "git remote add parent"
        git remote add -t master parent https://github.com/ninja-build/ninja
      fi
      git fetch parent
      git checkout "$branch"
      git rebase parent/master
    )
    git add subninja
  ) || exit 1
    D=$(cd volcano/vendor/subgn/subninja && git diff --cached)
    if [ -n "$D" ]; then
      echo "$item: please commit changes AND UPDATE subgn."
      exit 0
    fi
    ;;
  sync-subgn)
  (
    cd volcano/vendor/subgn
    if [ ! -x subninja/ninja ]; then
      echo "vendor/subninja/ninja missing. Please run volcanosamples/build.cmd."
      exit 1
    fi
    export PATH=$PATH:$PWD/subninja

    # Get the sources
    mkdir upstream-build
    cd upstream-build

    git_export tools https://chromium.googlesource.com/chromium/src/tools/gn

    mkdir -p third_party/libevent
    (
      cd third_party/libevent
      curl -k -O https://chromium.googlesource.com/chromium/chromium/+archive/master/third_party/libevent.tar.gz
      tar zxf libevent.tar.gz
      rm libevent.tar.gz
    )
    (
      # Only need some parts of third_party. Discard the rest.
      cd third_party
      git clone https://chromium.googlesource.com/chromium/src/third_party
      mv third_party/apple_apsl .
      rm -rf third_party
    )

    # https://chromium.googlesource.com/chromium/src/base that supported c++11
    # (HEAD now requires c++14).
    git clone https://chromium.googlesource.com/chromium/src/base
    (
      cd base
      git checkout 1a3f5088d6885d8411829cc2ffdcb3d007135481
    )
    git_export . https://chromium.googlesource.com/chromium/src/build
    ln -s build/config config
    git_export testing https://chromium.googlesource.com/chromium/testing/gtest
    find . -name .gitignore -o -name git-hooks -exec rm -r {} +

    # Build from source. If this fails, manual fixing is required.
    patch -p1 -i ../subgn.patch
    (
      cd tools/gn
      bootstrap/bootstrap.py -s --no-clean
    )
    # Build successful. Clean build results.
    rm -r out out_bootstrap
  ) || exit 1
  (
    cd volcano/vendor/subgn
    rm -r base build config testing third_party tools
    ( cd upstream-build && tar cf - . | tar xf - -C .. )
    rm -r upstream-build
    git add .
  ) || exit 1
    D=$(cd volcano/vendor/subgn && git diff --cached)
    if [ -n "$D" ]; then
      echo "$item: please commit changes AND UPDATE volcano."
      exit 0
    fi
    ;;
  sync-skia)
    # TODO: pull but squash history, have it ready to commit.
    ;;
  sync-vulkansamples)
  (
    cd volcano/vendor/vulkansamples
    git checkout HEAD -- .
    git checkout master
    git pull
    cd ..
    git add vulkansamples
  ) || exit 1
    D=$(cd volcano/vendor/vulkansamples && git diff --cached)
    if [ -n "$D" ]; then
      echo "$item: please test build, then commit AND UPDATE volcano."
      exit 0
    fi
    ;;
  sync-glfw)
  (
    cd volcano/vendor/glfw
    git checkout HEAD -- .
    git checkout master
    git pull
    cd ..
    git add glfw
  ) || exit 1
    D=$(cd volcano/vendor/glfw && git diff --cached)
    if [ -n "$D" ]; then
      echo "$item: please test build, then commit AND UPDATE volcano."
      exit 0
    fi
    ;;
  sync-spirv-cross)
  (
    cd volcano/vendor/spirv_cross
    git checkout HEAD -- .
    git checkout master
    git pull
    cd ..
    git add spirv_cross
  ) || exit 1
    D=$(cd volcano/vendor/spirv_cross && git diff --cached)
    if [ -n "$D" ]; then
      echo "$item: please test build, then commit AND UPDATE volcano."
      exit 0
    fi
    ;;
  sync-assimp)
  (
    cd assimp
    # Roll back "ISO C++ says..." patch, if found.
    T="$(git log --format=%B -n1 | head -n1)"
    if [ "$T" == "Fix \"ISO C++ says that these are ambiguous\"" ]; then
      BACK_TO="$(git status | head -n1)"
      git checkout HEAD~1
    fi
    git_export upstream-build https://github.com/assimp/assimp
    rm -rf upstream-build/assimp/test
    ( cd upstream-build/assimp && tar cf - . ) | tar xf -
    rm -rf upstream-build
    git add .
    D=$(git diff --cached)
    if [ -n "$D" ]; then
      echo "$item: please test build, commit, then re-run. PATCH NOT APPLIED."
      exit 1
    fi
    if [ -n "$BACK_TO" ]; then
      if [ "${BACK_TO#HEAD detached at }" != "$BACK_TO" ]; then
        BACK_TO="${BACK_TO#HEAD detached at }"
      elif [ "${BACK_TO#On branch }" != "$BACK_TO" ]; then
        BACK_TO="${BACK_TO#On branch }"
      else
        echo "Trying to restore the vendor/assimp HEAD failed:"
        echo "$BACK_TO"
        exit 1
      fi
      git checkout "$BACK_TO"
    fi
    # Apply patch. This is a no-op if git checkout "$BACK_TO" succeeded.
    find . \( -name '*.cpp' -o -name '*.inl' \) -exec \
        sed -i -e 's/(\([^ ]*[Oo]ut[Ff]ile\) == \(NULL\|0\)/(!\1/' {} +
    patch --no-backup-if-mismatch -Nu -p0 <<EOF
--- code/Importer.cpp   2017-09-04 18:37:44.724972574 -0700
+++ code/Importer.cpp   2017-09-07 09:09:22.263197901 -0700
@@ -813,7 +813,7 @@
 
 #ifdef ASSIMP_BUILD_NO_VALIDATEDS_PROCESS
         continue;
-#endif  // no validation
+#else
 
         // If the extra verbose mode is active, execute the ValidateDataStructureStep again - after each step
         if (pimpl->bExtraVerbose)   {
@@ -826,6 +826,7 @@
                 break;
             }
         }
+#endif  // no validation
 #endif // ! DEBUG
     }
     pimpl->mProgressHandler->UpdatePostProcess( static_cast<int>(pimpl->mPostProcessingSteps.size()), static_cast<int>(pimpl->mPostProcessingSteps.size()) );
@@ -900,11 +901,13 @@
     if ( pimpl->bExtraVerbose || requestValidation  ) {
         DefaultLogger::get()->debug( "Verbose Import: revalidating data structures" );
 
+#ifndef ASSIMP_BUILD_NO_VALIDATEDS_PROCESS
         ValidateDSProcess ds;
         ds.ExecuteOnScene( this );
         if ( !pimpl->mScene ) {
             DefaultLogger::get()->error( "Verbose Import: failed to revalidate data structures" );
         }
+#endif
     }
 
     // clear any data allocated by post-process steps
EOF
    if [ -f code/*.rej ] || [ -f code/*.orig ]; then
      echo "Patch left stray .rej or .orig files. Please clean them up."
      exit 1
    fi
    git add .
    D=$(git diff --cached)
    if [ -n "$D" ]; then
      git branch -f master
      git checkout master
      git commit -m "Fix \"ISO C++ says that these are ambiguous\"

Comparing a unique_ptr to NULL or 0 produces lots of warning text
on some platforms.

Also fix code that should be within ASSIMP_BUILD_NO_VALIDATEDS_PROCESS."
      echo "$item: please test build, commit, then re-run. PATCH NOT APPLIED."
      exit 1
    fi
  ) || exit 1
    ;;
  sync-cereal)
  (
    cd volcano/vendor/cereal
    # undo local patches
    git checkout HEAD -- .
    # switch from detached HEAD to branch master
    git checkout master
    # fast-forward to new HEAD
    git pull
    cd ..
    git add cereal
  ) || exit 1
    D=$(cd volcano && git diff --cached vendor/cereal)
    if [ -n "$D" ]; then
      echo "$item: please test build, then commit AND UPDATE volcano."
      exit 0
    fi
    ;;
  sync-vulkanmemoryallocator)
    VMA_TGZ=/tmp/vulkanmemoryallocator/vma.tgz
  (
    rm -rf /tmp/vulkanmemoryallocator
    mkdir /tmp/vulkanmemoryallocator
    cd /tmp/vulkanmemoryallocator
    git clone --depth 5 \
        https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator orig
    cd orig/src
    rm -rf Shaders VulkanSample.cpp Common.cpp Common.h Tests.cpp Tests.h
    rm -f VmaUsage.cpp VmaUsage.h vk_mem_alloc.natvis
    git log -1 --pretty="%H" > COMMIT_HASH
    tar zcf "$VMA_TGZ" .
  ) || exit 1
  (
    cd volcano/vendor
    rm -rf vulkanmemoryallocator
    mkdir vulkanmemoryallocator
    cd vulkanmemoryallocator
    tar zxf "$VMA_TGZ"
    rm -f "$VMA_TGZ"
    git add -- .
  ) || exit 1
    rm -rf /tmp/vulkanmemoryallocator
    D=$(cd volcano && git diff --cached vendor/vulkanmemoryallocator)
    if [ -n "$D" ]; then
      echo "$item: please test build, then commit AND UPDATE volcano."
      exit 0
    fi
    ;;
  *)
    echo "todo contains unknown item=$item"
    exit 1
  esac

  echo ""
  echo "------ END: $item ------"
done

echo "Everything is up-to-date."
