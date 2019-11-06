#!/bin/bash
set -e

cd -P -- "$(dirname -- "$0")"

VOLCANO_REPO_URL="https://github.com/ndsol/volcano"

echo "Test: clone volcano into a tmpdir, delete .git and build"
D=$(mktemp --tmpdir -d integration-testXXXXXXXX)
cd "$D"
git clone "$VOLCANO_REPO_URL"
rm -rf volcano/.git
volcano/build.cmd
rm -r "$D"

echo "Test: delete volcano and re-clone from scratch"
(
  cd vendor
  rm -rf volcano
  git rm -f volcano || true
)
(
  cd .git/modules/vendor
  rm -rf volcano
)

(
  cd vendor
  git submodule add "$VOLCANO_REPO_URL"
)

./build.cmd
vendor/subgn/ninja -C out/Debug

echo "Test: build android"
echo "Test: launch app"
echo "Test: switch to home screen"
echo "Test: switch back to app"
echo "Test: long press on app, then press home button"

echo "Test: assimp dependencies check:"
rm -rf /tmp/s || true
mkdir /tmp/s
sed -e 's/.*\(use_assimp_[^ ]*\) = false$/\1/p;d' \
  $VASSIMP/formats.gni | \
while read a; do
  echo ""
  echo "$a"
  sed -i -e "s/$a = false/$a = true/" $VASSIMP/formats.gni
  vendor/subgn/ninja -C out/Debug 06threepipelines
  set +e
  out/Debug/06threepipelines
  RV=$?
  (
    for b in ../../out/Debug/{obj/src/gn/vendor/assimp/libassimp.a,06threepipelines}
    do
      echo -n "$(basename $b) $(stat -c '%s' $b) "
    done
    echo "$RV"
  ) > /tmp/s/$a
  sed -i -e "s/$a = true/$a = false/" src/gn/vendor/assimp/formats.gni
  set -e
  if [ $RV -ne 0 ] && [ $RV -ne 20 ]; then
    echo "RV=$RV"
    break
  fi
done
